#include "sql/executor/update.h"
#include "catalog/catalog.h"
#include "common/tuple_key.h"
#include "container/hash_map.h"
#include "database/database.h"
#include "sql/executor/seq_scan.h"
#include "sql/executor/expression_evaluator.h"
#include "transaction/transaction.h"
#include "recovery/wal.h"
#include "sql/parser/ast.h"
#include "sql/parser/parser.h"

namespace minidb {

static bool btree_supports_type(TypeId type) {
    return type == TypeId::kBool || type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble ||
           type == TypeId::kVarchar || type == TypeId::kTimestamp ||
           type == TypeId::kDatetime || type == TypeId::kNull;
}

static bool is_datetime_type(TypeId type) {
    return type == TypeId::kTimestamp || type == TypeId::kDatetime;
}

static Value cast_value_for_column(const Value& value, TypeId target) {
    if (value.is_null() || value.type_id() == target) return value;
    if (!is_datetime_type(target)) return value;
    Value casted = value.cast_to(target);
    return casted.is_null() ? value : casted;
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
// proposed post-UPDATE row. Returns nullptr when all CHECKs hold, otherwise
// a static reason string. NULL result is treated as UNKNOWN (passes), per
// SQL standard CHECK semantics.
static const char* check_constraint_violation(const Schema& schema,
                                              const Vector<Value>& row) {
    Tuple candidate(schema, row);
    for (u32 c = 0; c < schema.column_count(); c++) {
        const Column& col = schema.get_column(c);
        if (col.check_expr.empty()) continue;
        String wrapped("SELECT ");
        wrapped += col.check_expr;
        Parser p(wrapped);
        Statement stmt = p.parse();
        if (!p.ok() || !stmt.select || stmt.select->select_list.empty()) {
            return "CHECK constraint expression invalid";
        }
        Value result = ExpressionEvaluator::evaluate(*stmt.select->select_list[0], candidate);
        if (result.is_null()) continue;
        bool ok = false;
        if (result.type_id() == TypeId::kBool) {
            ok = result.get_bool();
        } else if (result.type_id() == TypeId::kInt32) {
            ok = result.get_int32() != 0;
        } else if (result.type_id() == TypeId::kInt64) {
            ok = result.get_int64() != 0;
        } else {
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

static String record_id_key(const RecordId& rid) {
    String key(static_cast<u64>(rid.page_id));
    key += ':';
    key += String(static_cast<u64>(rid.slot_idx));
    return key;
}

static bool group_intersects_columns(const Vector<u32>& group, const Vector<u32>& cols) {
    for (u32 i = 0; i < group.size(); i++) {
        for (u32 j = 0; j < cols.size(); j++) {
            if (group[i] == cols[j]) return true;
        }
    }
    return false;
}

UpdateExecutor::UpdateExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                               Vector<Pair<String, UniquePtr<Expression>>> set_clauses,
                               UniquePtr<Executor> child,
                               TransactionManager* txn_mgr, u32 table_id,
                               WalManager* wal, Catalog* catalog, Database* db)
    : pool_(pool), heap_(heap), schema_(schema),
      set_clauses_(static_cast<Vector<Pair<String, UniquePtr<Expression>>>&&>(set_clauses)),
      child_(static_cast<UniquePtr<Executor>&&>(child)), executed_(false),
      txn_mgr_(txn_mgr), table_id_(table_id), wal_(wal), catalog_(catalog), db_(db) {
    Column col;
    col.name = "affected_rows";
    col.type = TypeId::kInt32;
    result_schema_.add_column(col);
}

void UpdateExecutor::init() {
    child_->init();
    executed_ = false;
}

bool UpdateExecutor::row_satisfies_schema(const Vector<Value>& row) const {
    // Defers to Schema::validate_row so NOT NULL, VARCHAR(n), and future
    // CHECK constraints all live in one place. The caller surfaces the
    // returned message via set_executor_error.
    return schema_.validate_row(row) == nullptr;
}

bool UpdateExecutor::violates_unique_constraints(const Vector<Value>& row,
                                                 const RecordId& self_rid,
                                                 const Vector<Vector<u32>>& unique_groups) const {
    if (!catalog_) return false;
    TableEntry* table = catalog_->get_table(table_id_);
    if (!table) return false;
    Vector<IndexEntry*> indexes = catalog_->get_indexes(table_id_);
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
                    if (matches[m] == self_rid) continue;
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
            if (page_id == self_rid.page_id && slot == self_rid.slot_idx) continue;

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

ExecResult UpdateExecutor::next() {
    if (executed_) return ExecResult::empty();
    executed_ = true;

    bool explicit_txn = txn_mgr_ && txn_mgr_->current();
    u64 txn_id = explicit_txn ? txn_mgr_->current()->id() : reinterpret_cast<u64>(this);
    Vector<RecordId> autocommit_record_locks;

    // Collect column indices modified by SET clause
    Vector<u32> modified_cols;
    for (u32 i = 0; i < set_clauses_.size(); i++) {
        int idx = schema_.get_column_index(set_clauses_[i].first);
        if (idx >= 0) modified_cols.push_back(static_cast<u32>(idx));
    }

    // Check if HOT update is possible (no indexed columns modified)
    bool hot_eligible = catalog_ ? !catalog_->any_column_indexed(table_id_, modified_cols) : false;

    Vector<Vector<u32>> all_unique_groups;
    for (u32 i = 0; i < schema_.column_count(); i++) {
        if (schema_.get_column(i).is_primary || schema_.get_column(i).is_unique) {
            Vector<u32> cols;
            cols.push_back(i);
            all_unique_groups.push_back(static_cast<Vector<u32>&&>(cols));
        }
    }
    if (catalog_) {
        Vector<IndexEntry*> indexes = catalog_->get_indexes(table_id_);
        for (u32 i = 0; i < indexes.size(); i++) {
            if (indexes[i] && indexes[i]->is_unique) {
                all_unique_groups.push_back(indexes[i]->key_columns);
            }
        }
    }
    Vector<Vector<u32>> unique_groups;
    for (u32 g = 0; g < all_unique_groups.size(); g++) {
        if (group_intersects_columns(all_unique_groups[g], modified_cols)) {
            unique_groups.push_back(all_unique_groups[g]);
        }
    }
    HashMap<String, bool> pending_unique_keys;

    // Drain the WHERE-side iterator into a frozen RID list BEFORE applying
    // any updates. Streaming directly from `child_->next()` while mutating
    // the heap is unsafe: the HOT path writes a new same-page version that
    // satisfies "own write" visibility, so a subsequent child_->next() would
    // see it and re-match the predicate, looping until the page fills up
    // (the classic SQL "Halloween problem"). Materialising decouples scan
    // from mutation in both the HOT and non-HOT branches.
    Vector<RecordId> materialized_targets;
    {
        HashMap<String, bool> seen_targets;
        while (true) {
            if (executor_cancelled()) break;
            ExecResult result = child_->next();
            if (!result.ok()) break;

            RecordId rid;
            if (!child_->last_record_id(&rid)) continue;
            String rid_key = record_id_key(rid);
            if (seen_targets.find(rid_key)) continue;
            seen_targets.insert(rid_key, true);
            materialized_targets.push_back(rid);
        }
    }

    u32 count = 0;
    u32 materialized_pos = 0;
    while (true) {
        if (executor_cancelled()) break;
        if (materialized_pos >= materialized_targets.size()) break;
        RecordId old_rid = materialized_targets[materialized_pos++];
        Tuple old_tuple;
        if (!db_ || !db_->read_tuple(table_id_, schema_, old_rid, &old_tuple)) continue;
        // No silent skip on xmax!=0 here. A non-zero xmax means another
        // transaction modified the row between our snapshot and now — the
        // lock + xmax recheck below surfaces that as a serialization error
        // (I2). Silently skipping would mask the lost-update conflict.

        Vector<Value> new_values;
        for (u32 i = 0; i < old_tuple.column_count(); i++) {
            new_values.push_back(old_tuple.get_value(i));
        }

        for (u32 i = 0; i < set_clauses_.size(); i++) {
            const String& col_name = set_clauses_[i].first;
            int idx = schema_.get_column_index(col_name);
            if (idx < 0) continue;
            Value new_val = ExpressionEvaluator::evaluate(*set_clauses_[i].second, old_tuple);
            TypeId target_type = schema_.get_column(static_cast<u32>(idx)).type;
            new_values[static_cast<u32>(idx)] = cast_value_for_column(new_val, target_type);
        }

        if (db_ && !db_->lock_manager().lock_record(txn_id, table_id_, old_rid,
                                                     LockMode::kRowExclusive).ok()) {
            set_executor_error("could not serialize access due to concurrent update");
            return ExecResult::empty();
        }
        if (!explicit_txn) autocommit_record_locks.push_back(old_rid);

        // Write-write conflict detection: after taking the row lock, re-read
        // xmax to make sure another transaction did not modify or delete this
        // row in the window between our snapshot and our lock acquisition.
        {
            auto pg = pool_->fetch_page(old_rid.page_id);
            if (!pg.ok()) {
                set_executor_error("could not read row for update");
                return ExecResult::empty();
            }
            Page* p = pg.value();
            const LinePointer* lp = p->line_pointer(old_rid.slot_idx);
            if (!lp || !lp->is_valid() || lp->offset + 16 > kPageSize) {
                pool_->unpin_page(old_rid.page_id);
                set_executor_error("could not serialize access due to concurrent update");
                return ExecResult::empty();
            }
            u64 cur_xmax = 0;
            std::memcpy(&cur_xmax, p->data() + lp->offset + 8, 8);
            pool_->unpin_page(old_rid.page_id);
            if (cur_xmax != kInvalidTxnId && cur_xmax != txn_id) {
                set_executor_error("could not serialize access due to concurrent update");
                return ExecResult::empty();
            }
        }

        if (const char* reason = schema_.validate_row(new_values)) {
            set_executor_error(reason);
            return ExecResult::empty();
        }
        if (const char* reason = check_constraint_violation(schema_, new_values)) {
            set_executor_error(reason);
            return ExecResult::empty();
        }
        if (!unique_groups.empty() &&
            violates_unique_constraints(new_values, old_rid, unique_groups)) {
            continue;
        }
        bool batch_unique_conflict = false;
        Vector<String> row_unique_keys;
        for (u32 g = 0; g < unique_groups.size(); g++) {
            String key;
            if (!make_projected_values_key(new_values, unique_groups[g], true, &key)) continue;
            String scoped = String(static_cast<u64>(g));
            scoped += '|';
            scoped += key;
            if (pending_unique_keys.find(scoped)) {
                batch_unique_conflict = true;
                break;
            }
            row_unique_keys.push_back(scoped);
        }
        if (batch_unique_conflict) continue;

        // Build new version Tuple
        // Version chain direction: old -> new -> end (PostgreSQL t_ctid).
        // New version's next_ver = (0,0) signals end of chain.
        Tuple new_tuple(schema_, new_values);
        new_tuple.set_xmin(txn_id);
        new_tuple.set_xmax(0);
        new_tuple.set_next_version(kNullPageId, 0);  // end of chain
        if (db_ && !db_->validate_index_keys(table_id_, new_tuple)) continue;

        u32 serialized_size = new_tuple.serialized_size();
        if (serialized_size > kPageSize) continue;
        byte buffer[kPageSize];
        byte* buf = new_tuple.serialize_to_page(buffer);
        u16 size = static_cast<u16>(buf - buffer);

        bool hot_used = false;

        // ============================================================
        // HOT Update: WAL-first — reserve slot (RAII), write WAL, commit.
        // ============================================================
        if (hot_eligible) {
            auto prepare = heap_->prepare_insert_in_page(old_rid.page_id, size);
            if (prepare.ok()) {
                auto reservation = std::move(prepare.value());
                u64 lsn = 0;
                if (wal_) {
                    lsn = wal_->log_update(txn_id, table_id_,
                                           old_rid.page_id, old_rid.slot_idx,
                                           old_rid.page_id, reservation.predicted_slot(),
                                           buffer, size);
                }

                auto hot_result = reservation.commit(buffer, size, lsn);
                if (hot_result.ok()) {
                    for (u32 k = 0; k < row_unique_keys.size(); k++) {
                        pending_unique_keys.insert(row_unique_keys[k], true);
                    }
                    Pair<PageId, SlotIdx> new_rid = hot_result.value();

                    // Atomic: set_next_version + mark_deleted + set_lsn (LSN stamped before unpin).
                    heap_->commit_old_tuple(old_rid.page_id, old_rid.slot_idx,
                                            new_rid.first, new_rid.second, txn_id, lsn);

                    if (txn_mgr_ && txn_mgr_->current()) {
                        RecordId new_record_id(new_rid.first, new_rid.second);
                        txn_mgr_->record_hot_delete(table_id_, old_rid);
                        txn_mgr_->record_hot_insert(table_id_, new_record_id);
                    }
                    hot_used = true;
                    count++;
                }
            }
        }

        // ============================================================
        // Non-HOT fallback: WAL-first — reserve slot (RAII), write WAL, commit.
        // ============================================================
        if (!hot_used) {
            auto prepare = heap_->prepare_insert(size);
            if (prepare.ok()) {
                auto reservation = std::move(prepare.value());
                u64 lsn = 0;
                if (wal_) {
                    lsn = wal_->log_update(txn_id, table_id_,
                                           old_rid.page_id, old_rid.slot_idx,
                                           reservation.page_id(), reservation.predicted_slot(),
                                           buffer, size);
                }

                auto ins_result = reservation.commit(buffer, size, lsn);
                if (ins_result.ok()) {
                    Pair<PageId, SlotIdx> new_rid = ins_result.value();
                    RecordId new_record_id(new_rid.first, new_rid.second);

                    // Atomic: set_next_version + mark_deleted + set_lsn.
                    heap_->commit_old_tuple(old_rid.page_id, old_rid.slot_idx,
                                            new_rid.first, new_rid.second, txn_id, lsn);

                    // Record undo BEFORE touching indexes so a failure in
                    // insert_index_entries unwinds heap + partial indexes
                    // via the transaction's rollback path.
                    if (txn_mgr_ && txn_mgr_->current()) {
                        txn_mgr_->record_delete(table_id_, old_rid);
                        txn_mgr_->record_insert(table_id_, new_record_id);
                    }
                    if (db_) {
                        // Do NOT eagerly remove the old version's index
                        // entry — under SI an older snapshot may still need
                        // to find the old row through this index. GC removes
                        // it later. Insert the new version's entries
                        // incrementally so non-HOT UPDATE does not rebuild
                        // every table index after each statement.
                        if (!db_->insert_index_entries(table_id_, new_tuple, new_record_id)) {
                            set_executor_error("index insert failed");
                            return ExecResult::empty();
                        }
                    }
                    for (u32 k = 0; k < row_unique_keys.size(); k++) {
                        pending_unique_keys.insert(row_unique_keys[k], true);
                    }
                    count++;
                }
            }
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

const Schema& UpdateExecutor::output_schema() const { return result_schema_; }

} // namespace minidb
