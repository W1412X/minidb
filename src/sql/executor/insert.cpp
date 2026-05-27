#include "sql/executor/insert.h"
#include "catalog/catalog.h"
#include "common/tuple_key.h"
#include "container/hash_map.h"
#include "database/database.h"
#include "transaction/transaction.h"
#include "recovery/wal.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/parser.h"
#include "index/btree.h"
#include <cstring>

namespace minidb {

static bool btree_supports_type(TypeId type) {
    return type == TypeId::kBool || type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble ||
           type == TypeId::kVarchar || type == TypeId::kTimestamp ||
           type == TypeId::kDatetime || type == TypeId::kNull;
}

static bool same_key_columns(const Vector<u32>& left, const Vector<u32>& right) {
    if (left.size() != right.size()) return false;
    for (u32 i = 0; i < left.size(); i++) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

static bool build_unique_index_key(const Schema& schema, const Vector<Value>& row,
                                   const Vector<u32>& columns, IndexKey* key) {
    if (!key || columns.empty()) return false;
    Vector<Value> values;
    for (u32 i = 0; i < columns.size(); i++) {
        u32 col = columns[i];
        if (col >= schema.column_count() || col >= row.size() ||
            !btree_supports_type(schema.get_column(col).type) ||
            row[col].is_null()) {
            return false;
        }
        values.push_back(row[col]);
    }
    *key = IndexKey::from_values(values);
    return key->fits();
}

// Evaluate every column-level CHECK declared on this schema against the
// candidate row. Returns nullptr when all CHECKs hold, otherwise a static
// reason string suitable for set_executor_error. SQL semantics: a CHECK
// fails only when the predicate evaluates to FALSE — NULL passes (UNKNOWN
// is permitted), matching PostgreSQL / SQL standard behaviour.
static const char* check_constraint_violation(const Schema& schema,
                                              const Vector<Value>& row) {
    Tuple candidate(schema, row);
    for (u32 c = 0; c < schema.column_count(); c++) {
        const Column& col = schema.get_column(c);
        if (col.is_dropped) continue;
        if (col.check_expr.empty()) continue;
        String wrapped("SELECT ");
        wrapped += col.check_expr;
        Parser p(wrapped);
        Statement stmt = p.parse();
        if (!p.ok() || !stmt.select || stmt.select->select_list.empty()) {
            return "CHECK constraint expression invalid";
        }
        Value result = ExpressionEvaluator::evaluate(*stmt.select->select_list[0], candidate);
        if (result.is_null()) continue;  // NULL → UNKNOWN → passes.
        bool ok = false;
        if (result.type_id() == TypeId::kBool) {
            ok = result.get_bool();
        } else if (result.type_id() == TypeId::kInt32) {
            ok = result.get_int32() != 0;
        } else if (result.type_id() == TypeId::kInt64) {
            ok = result.get_int64() != 0;
        } else {
            // Non-boolean, non-integer CHECK result: refuse the row rather
            // than silently passing — callers should write predicates that
            // produce a boolean.
            return "CHECK constraint produced non-boolean";
        }
        if (!ok) return "CHECK constraint violated";
    }
    return nullptr;
}

static bool tuple_live_for_unique_check(const Tuple& tuple, TransactionManager* txn_mgr) {
    if (tuple.xmax() == kInvalidTxnId) return true;
    Transaction* current = txn_mgr ? txn_mgr->current() : nullptr;
    if (!current) return false;
    if (tuple.xmax() == current->id()) return false;

    TxnState state;
    if (txn_mgr->get_txn_state(tuple.xmax(), &state)) {
        return state != TxnState::kCommitted;
    }
    return tuple.xmax() >= current->snapshot_id();
}

InsertExecutor::InsertExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                               const Vector<Vector<Value>>& values,
                               TransactionManager* txn_mgr, u32 table_id,
                               WalManager* wal, Catalog* catalog, Database* db)
    : pool_(pool), heap_(heap), schema_(schema), values_(values), executed_(false),
      txn_mgr_(txn_mgr), table_id_(table_id), wal_(wal), catalog_(catalog), db_(db) {
    Column col;
    col.name = "affected_rows";
    col.type = TypeId::kInt32;
    result_schema_.add_column(col);
}

void InsertExecutor::init() { executed_ = false; }

bool InsertExecutor::row_satisfies_schema(const Vector<Value>& row) const {
    // Defers to Schema::validate_row so NOT NULL, VARCHAR(n), and future
    // CHECK constraints all live in one place. The caller surfaces the
    // returned message via set_executor_error.
    return schema_.validate_row(row) == nullptr;
}

bool InsertExecutor::violates_unique_constraints(const Vector<Value>& row) const {
    if (!catalog_) return false;
    TableEntry* table = catalog_->get_table(table_id_);
    if (!table) return false;

    Vector<Vector<u32>> unique_groups;
    for (u32 i = 0; i < schema_.column_count(); i++) {
        const Column& col = schema_.get_column(i);
        if (col.is_primary) {
            Vector<u32> cols;
            cols.push_back(i);
            unique_groups.push_back(static_cast<Vector<u32>&&>(cols));
        } else if (col.is_unique) {
            Vector<u32> cols;
            cols.push_back(i);
            unique_groups.push_back(static_cast<Vector<u32>&&>(cols));
        }
    }

    Vector<IndexEntry*> indexes = catalog_->get_indexes(table_id_);
    for (u32 i = 0; i < indexes.size(); i++) {
        if (indexes[i] && indexes[i]->is_unique) {
            unique_groups.push_back(indexes[i]->key_columns);
        }
    }

    if (unique_groups.empty()) return false;

    Vector<Vector<u32>> heap_checked_groups;
    for (u32 g = 0; g < unique_groups.size(); g++) {
        const Vector<u32>& group = unique_groups[g];
        bool checked_by_index = false;

        IndexKey lookup_key;
        if (db_ && build_unique_index_key(schema_, row, group, &lookup_key)) {
            for (u32 i = 0; i < indexes.size(); i++) {
                IndexEntry* index = indexes[i];
                if (!index || !index->is_unique || !same_key_columns(index->key_columns, group)) {
                    continue;
                }

                BPlusTree* tree = db_->get_index_tree(index->index_id);
                if (!tree) break;

                Vector<RecordId> matches = tree->search(lookup_key);
                for (u32 m = 0; m < matches.size(); m++) {
                    Tuple existing;
                    if (!db_->read_tuple(table_id_, schema_, matches[m], &existing)) continue;
                    if (tuple_live_for_unique_check(existing, txn_mgr_)) {
                        return true;
                    }
                }
                checked_by_index = true;
                break;
            }
        }

        if (!checked_by_index) {
            heap_checked_groups.push_back(group);
        }
    }

    if (heap_checked_groups.empty()) return false;

    PageId first_page = heap_->first_data_page_id();
    if (first_page == kNullPageId) return false;
    u32 file_id = file_id_from_page(first_page);
    u32 page_num = page_num_from_page(first_page);
    u32 pages = heap_->meta().num_data_pages;

    for (u32 p = 0; p < pages; p++, page_num++) {
        PageId page_id = make_page_id(file_id, page_num);
        auto result = pool_->fetch_page(page_id, true);
        if (!result.ok()) continue;

        Page* page = result.value();
        u16 num_tuples = page->header()->num_tuples;

        for (u16 slot = 0; slot < num_tuples; slot++) {
            const LinePointer* lp = page->line_pointer(slot);
            if (!lp || !lp->is_valid()) continue;

            Tuple tuple = Tuple::deserialize_from_page(page->data() + lp->offset, schema_, lp->length);
            if (!tuple_live_for_unique_check(tuple, txn_mgr_)) continue;
            for (u32 g = 0; g < heap_checked_groups.size(); g++) {
                bool same = true;
                for (u32 c = 0; c < heap_checked_groups[g].size(); c++) {
                    u32 col_idx = heap_checked_groups[g][c];
                    if (row[col_idx].is_null() || tuple.get_value(col_idx).is_null() ||
                        row[col_idx] != tuple.get_value(col_idx)) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    pool_->unpin_page(page_id);
                    return true;
                }
            }
        }

        pool_->unpin_page(page_id);
    }
    return false;
}

ExecResult InsertExecutor::next() {
    if (executed_) return ExecResult::empty();
    executed_ = true;

    u32 count = 0;
    bool explicit_txn = txn_mgr_ && txn_mgr_->current();
    u64 txn_id = explicit_txn ? txn_mgr_->current()->id() : reinterpret_cast<u64>(this);
    Vector<String> autocommit_key_locks;
    Vector<RecordId> autocommit_record_locks;
    Vector<Vector<u32>> unique_groups;
    for (u32 c = 0; c < schema_.column_count(); c++) {
        const Column& col = schema_.get_column(c);
        if (col.is_primary || col.is_unique) {
            Vector<u32> cols;
            cols.push_back(c);
            unique_groups.push_back(static_cast<Vector<u32>&&>(cols));
        }
    }
    if (catalog_) {
        Vector<IndexEntry*> indexes = catalog_->get_indexes(table_id_);
        for (u32 idx = 0; idx < indexes.size(); idx++) {
            if (indexes[idx] && indexes[idx]->is_unique) {
                unique_groups.push_back(indexes[idx]->key_columns);
            }
        }
    }
    HashMap<String, bool> pending_unique_keys;

    // W15: If there are unique constraints without B+ tree indexes,
    // we need a table-level lock to ensure atomicity of the heap scan check.
    bool need_table_lock = false;
    if (catalog_) {
        Vector<IndexEntry*> indexes = catalog_->get_indexes(table_id_);
        for (u32 c = 0; c < schema_.column_count(); c++) {
            const Column& col = schema_.get_column(c);
            if (!col.is_primary && !col.is_unique) continue;
            // Check if there's a B+ tree index covering this column
            bool has_index = false;
            for (u32 idx = 0; idx < indexes.size(); idx++) {
                if (indexes[idx] && indexes[idx]->is_unique &&
                    indexes[idx]->key_columns.size() == 1 &&
                    indexes[idx]->key_columns[0] == c) {
                    has_index = true;
                    break;
                }
            }
            if (!has_index) { need_table_lock = true; break; }
        }
    }
    if (need_table_lock && db_) {
        db_->lock_manager().lock_table(txn_id, table_id_, LockMode::kExclusive);
    }

    for (u32 i = 0; i < values_.size(); i++) {
        if (const char* reason = schema_.validate_row(values_[i])) {
            set_executor_error(reason);
            return ExecResult::empty();
        }
        if (const char* reason = check_constraint_violation(schema_, values_[i])) {
            set_executor_error(reason);
            return ExecResult::empty();
        }
        Vector<String> row_unique_keys;
        bool key_lock_failed = false;
        for (u32 g = 0; g < unique_groups.size(); g++) {
            String key;
            if (!make_projected_values_key(values_[i], unique_groups[g], true, &key)) continue;
            String scoped = String(static_cast<u64>(g));
            scoped += '|';
            scoped += key;
            if (db_ && !db_->lock_manager().lock_key(txn_id, table_id_, scoped,
                                                       LockMode::kRowExclusive).ok()) {
                key_lock_failed = true;
                break;
            }
            if (!explicit_txn) autocommit_key_locks.push_back(scoped);
            row_unique_keys.push_back(scoped);
        }
        if (key_lock_failed) continue;
        if (violates_unique_constraints(values_[i])) continue;

        bool batch_unique_conflict = false;
        for (u32 k = 0; k < row_unique_keys.size(); k++) {
            if (pending_unique_keys.find(row_unique_keys[k])) {
                batch_unique_conflict = true;
                break;
            }
        }
        if (batch_unique_conflict) continue;

        Tuple tuple(schema_, values_[i]);
        tuple.set_xmin(txn_id);
        tuple.set_xmax(0);
        if (db_ && !db_->validate_index_keys(table_id_, tuple)) continue;

        u32 serialized_size = tuple.serialized_size();
        if (serialized_size > kPageSize) continue;
        byte buffer[kPageSize];
        byte* buf = tuple.serialize_to_page(buffer);
        u16 size = static_cast<u16>(buf - buffer);

        // WAL-first: 1. Reserve position (RAII) → 2. Write WAL → 3. Commit data
        auto prepare = heap_->prepare_insert(size);
        if (!prepare.ok()) continue;

        auto reservation = std::move(prepare.value());
        RecordId predicted_rid(reservation.page_id(), reservation.predicted_slot());
        if (db_ && !db_->lock_manager().lock_record(txn_id, table_id_, predicted_rid,
                                                     LockMode::kRowExclusive).ok()) {
            continue;  // reservation destructor releases the heap latch
        }
        if (!explicit_txn) autocommit_record_locks.push_back(predicted_rid);
        u64 lsn = 0;
        if (wal_) {
            lsn = wal_->log_insert(txn_id, table_id_, reservation.page_id(),
                                    reservation.predicted_slot(), buffer, size);
        }

        auto result = reservation.commit(buffer, size, lsn);
        if (result.ok()) {
            Pair<PageId, SlotIdx> rid = result.value();
            RecordId record_id(rid.first, rid.second);
            // Record the heap-insert undo BEFORE touching indexes so the
            // active transaction's rollback removes both the heap row and
            // any partial index entries via delete_index_entries.
            if (txn_mgr_ && txn_mgr_->current()) {
                txn_mgr_->record_insert(table_id_, record_id);
            }
            if (db_ && !db_->insert_index_entries(table_id_, tuple, record_id)) {
                set_executor_error("index insert failed");
                return ExecResult::empty();
            }
            for (u32 k = 0; k < row_unique_keys.size(); k++) {
                pending_unique_keys.insert(row_unique_keys[k], true);
            }
            count++;
        }
    }

    Vector<Value> rv;
    rv.push_back(Value(static_cast<i32>(count)));
    if (db_ && !explicit_txn) {
        for (u32 i = 0; i < autocommit_record_locks.size(); i++) {
            db_->lock_manager().unlock_record(txn_id, table_id_, autocommit_record_locks[i]);
        }
        for (u32 i = 0; i < autocommit_key_locks.size(); i++) {
            db_->lock_manager().unlock_key(txn_id, table_id_, autocommit_key_locks[i]);
        }
        if (need_table_lock) {
            db_->lock_manager().unlock_table(txn_id, table_id_);
        }
    }
    return ExecResult::ok(Tuple(result_schema_, rv));
}

const Schema& InsertExecutor::output_schema() const { return result_schema_; }

} // namespace minidb
