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
                continue;
            }
            if (!explicit_txn) autocommit_record_locks.push_back(rid);

            // W6: Write-write conflict detection — re-read xmax after acquiring lock
            {
                auto pg = db_->pool().fetch_page(rid.page_id);
                if (pg.ok()) {
                    Page* p = pg.value();
                    const LinePointer* lp = p->line_pointer(rid.slot_idx);
                    if (lp && lp->is_valid() && lp->offset + 16 <= kPageSize) {
                        u64 cur_xmax = 0;
                        std::memcpy(&cur_xmax, p->data() + lp->offset + 8, 8);
                        db_->pool().unpin_page(rid.page_id);
                        if (cur_xmax != kInvalidTxnId && cur_xmax != txn_id) {
                            continue;  // Already modified by another transaction
                        }
                    } else {
                        db_->pool().unpin_page(rid.page_id);
                        continue;  // Row no longer exists
                    }
                } else {
                    continue;
                }
            }

            if (db_) {
                db_->delete_index_entries(table_id_, result.tuple, rid);
            }
            u64 lsn = wal_ ? wal_->log_delete(txn_id, table_id_, rid.page_id, rid.slot_idx) : 0;
            heap_->mark_deleted(rid.page_id, rid.slot_idx, txn_id, lsn);
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
