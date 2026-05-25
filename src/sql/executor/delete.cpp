#include "sql/executor/delete.h"
#include "sql/executor/seq_scan.h"
#include "database/database.h"
#include "transaction/transaction.h"
#include "recovery/wal.h"

namespace minidb {

DeleteExecutor::DeleteExecutor(BufferPool*, HeapFile* heap, UniquePtr<Executor> child,
                               TransactionManager* txn_mgr, u32 table_id,
                               WalManager* wal, Database* db)
    : heap_(heap), child_(static_cast<UniquePtr<Executor>&&>(child)),
      executed_(false), txn_mgr_(txn_mgr), table_id_(table_id), wal_(wal), db_(db) {
    Column col;
    col.name = "deleted_rows";
    col.type = TypeId::kInt32;
    result_schema_.add_column(col);
}

void DeleteExecutor::init() {
    child_->init();
    executed_ = false;
}

ExecResult DeleteExecutor::next() {
    if (executed_) return ExecResult::empty();
    executed_ = true;

    u32 count = 0;
    bool explicit_txn = txn_mgr_ && txn_mgr_->current();
    u64 txn_id = explicit_txn ? txn_mgr_->current()->id() : reinterpret_cast<u64>(this);
    Vector<RecordId> autocommit_record_locks;
    while (true) {
        if (executor_cancelled()) break;
        ExecResult result = child_->next();
        if (!result.ok()) break;

        RecordId rid;
        if (child_->last_record_id(&rid)) {
            if (db_ && !db_->lock_manager().lock_record(txn_id, table_id_, rid,
                                                         LockMode::kRowExclusive).ok()) {
                set_executor_error("could not serialize access due to concurrent update");
                return ExecResult::empty();
            }
            if (!explicit_txn) autocommit_record_locks.push_back(rid);

            // DELIBERATELY does NOT call db_->delete_index_entries here.
            // Under snapshot isolation, an older transaction may still need
            // to find this tuple via IndexScan; the entry stays until GC
            // can prove no live snapshot can see the row anymore. The heap
            // tuple's xmax is set below, and IndexScan filters by visibility.
            u64 lsn = wal_ ? wal_->log_delete(txn_id, table_id_, rid.page_id, rid.slot_idx) : 0;
            bool conflict = false;
            if (!heap_->mark_deleted_if_current(rid.page_id, rid.slot_idx,
                                                txn_id, lsn, &conflict)) {
                set_executor_error(conflict
                    ? "could not serialize access due to concurrent update"
                    : "could not read row for delete");
                return ExecResult::empty();
            }
            if (txn_mgr_ && txn_mgr_->current()) {
                txn_mgr_->record_delete(table_id_, rid);
            }
            count++;
        }
    }

    if (db_ && !explicit_txn) {
        for (u32 i = 0; i < autocommit_record_locks.size(); i++) {
            db_->lock_manager().unlock_record(txn_id, table_id_, autocommit_record_locks[i]);
        }
    }

    Vector<Value> rv;
    rv.push_back(Value(static_cast<i32>(count)));
    return ExecResult::ok(Tuple(result_schema_, rv));
}

const Schema& DeleteExecutor::output_schema() const { return result_schema_; }

} // namespace minidb
