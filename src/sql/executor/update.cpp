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

namespace minidb {

static bool btree_supports_type(TypeId type) {
    return type == TypeId::kBool || type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble ||
           type == TypeId::kVarchar || type == TypeId::kNull;
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
    if (row.size() != schema_.column_count()) return false;
    for (u32 i = 0; i < schema_.column_count(); i++) {
        const Column& col = schema_.get_column(i);
        if (col.not_null && row[i].is_null()) return false;
    }
    return true;
}

bool UpdateExecutor::violates_unique_constraints(const Vector<Value>& row,
                                                 const RecordId& self_rid) const {
    if (!catalog_) return false;
    TableEntry* table = catalog_->get_table(table_id_);
    if (!table) return false;

    Vector<Vector<u32>> unique_groups;
    for (u32 i = 0; i < schema_.column_count(); i++) {
        if (schema_.get_column(i).is_primary) {
            Vector<u32> cols;
            cols.push_back(i);
            unique_groups.push_back(static_cast<Vector<u32>&&>(cols));
        } else if (schema_.get_column(i).is_unique) {
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

    Vector<Vector<u32>> unique_groups;
    for (u32 i = 0; i < schema_.column_count(); i++) {
        if (schema_.get_column(i).is_primary || schema_.get_column(i).is_unique) {
            Vector<u32> cols;
            cols.push_back(i);
            unique_groups.push_back(static_cast<Vector<u32>&&>(cols));
        }
    }
    if (catalog_) {
        Vector<IndexEntry*> indexes = catalog_->get_indexes(table_id_);
        for (u32 i = 0; i < indexes.size(); i++) {
            if (indexes[i] && indexes[i]->is_unique) {
                unique_groups.push_back(indexes[i]->key_columns);
            }
        }
    }
    HashMap<String, bool> pending_unique_keys;
    Vector<RecordId> materialized_targets;

    if (!hot_eligible) {
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
        RecordId old_rid;
        Tuple old_tuple;
        if (hot_eligible) {
            ExecResult result = child_->next();
            if (!result.ok()) break;
            old_tuple = static_cast<Tuple&&>(result.tuple);
            if (!child_->last_record_id(&old_rid)) continue;
        } else {
            if (materialized_pos >= materialized_targets.size()) break;
            old_rid = materialized_targets[materialized_pos++];
            if (!db_ || !db_->read_tuple(table_id_, schema_, old_rid, &old_tuple)) continue;
            if (old_tuple.xmax() != kInvalidTxnId) continue;
        }

        Vector<Value> new_values;
        for (u32 i = 0; i < old_tuple.column_count(); i++) {
            new_values.push_back(old_tuple.get_value(i));
        }

        for (u32 i = 0; i < set_clauses_.size(); i++) {
            const String& col_name = set_clauses_[i].first;
            int idx = schema_.get_column_index(col_name);
            if (idx < 0) continue;
            Value new_val = ExpressionEvaluator::evaluate(*set_clauses_[i].second, old_tuple);
            new_values[static_cast<u32>(idx)] = new_val;
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

        if (!row_satisfies_schema(new_values)) {
            set_executor_error("NOT NULL constraint violated");
            return ExecResult::empty();
        }
        if (violates_unique_constraints(new_values, old_rid)) continue;
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
        // Version chain方向: old → new → end (对齐 PostgreSQL t_ctid)
        // 新版本 next_ver = (0,0) 表示链尾
        Tuple new_tuple(schema_, new_values);
        new_tuple.set_xmin(txn_id);
        new_tuple.set_xmax(0);
        new_tuple.set_next_version(kNullPageId, 0);  // 链尾
        if (db_ && !db_->validate_index_keys(table_id_, new_tuple)) continue;

        u32 serialized_size = new_tuple.serialized_size();
        if (serialized_size > kPageSize) continue;
        byte buffer[kPageSize];
        byte* buf = new_tuple.serialize_to_page(buffer);
        u16 size = static_cast<u16>(buf - buffer);

        bool hot_used = false;

        // ============================================================
        // HOT Update: WAL-first — 预定位置 → 写 WAL → 提交数据
        // ============================================================
        if (hot_eligible) {
            auto prepare = heap_->prepare_insert_in_page(old_rid.page_id, size);
            if (prepare.ok()) {
                SlotIdx predicted_slot = prepare.value();
                u64 lsn = 0;
                if (wal_) {
                    lsn = wal_->log_update(txn_id, table_id_,
                                           old_rid.page_id, old_rid.slot_idx,
                                           old_rid.page_id, predicted_slot,
                                           buffer, size);
                }

                auto hot_result = heap_->commit_insert_in_page(
                    old_rid.page_id, predicted_slot, buffer, size, lsn);
                if (hot_result.ok()) {
                    for (u32 k = 0; k < row_unique_keys.size(); k++) {
                        pending_unique_keys.insert(row_unique_keys[k], true);
                    }
                    Pair<PageId, SlotIdx> new_rid = hot_result.value();

                    // 原子化: set_next_version + mark_deleted + set_lsn (LSN 在 unpin 前Settings)
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
        // 非 HOT (回退路径): WAL-first — 预定位置 → 写 WAL → 提交数据
        // ============================================================
        if (!hot_used) {
            auto prepare = heap_->prepare_insert(size);
            if (prepare.ok()) {
                HeapFile::InsertPlan plan = prepare.value();
                u64 lsn = 0;
                if (wal_) {
                    lsn = wal_->log_update(txn_id, table_id_,
                                           old_rid.page_id, old_rid.slot_idx,
                                           plan.page_id, plan.predicted_slot,
                                           buffer, size);
                }

                auto ins_result = heap_->commit_insert(
                    plan.page_id, plan.is_new_page, plan.predicted_slot, buffer, size, lsn);
                if (ins_result.ok()) {
                    for (u32 k = 0; k < row_unique_keys.size(); k++) {
                        pending_unique_keys.insert(row_unique_keys[k], true);
                    }
                    count++;
                    Pair<PageId, SlotIdx> new_rid = ins_result.value();
                    RecordId new_record_id(new_rid.first, new_rid.second);

                    // 原子化: set_next_version + mark_deleted + set_lsn
                    heap_->commit_old_tuple(old_rid.page_id, old_rid.slot_idx,
                                            new_rid.first, new_rid.second, txn_id, lsn);

                    if (txn_mgr_ && txn_mgr_->current()) {
                        txn_mgr_->record_delete(table_id_, old_rid);
                        txn_mgr_->record_insert(table_id_, new_record_id);
                    }
                    if (db_ && hot_eligible) {
                        db_->delete_index_entries(table_id_, old_tuple, old_rid);
                        db_->insert_index_entries(table_id_, new_tuple, new_record_id);
                    }
                }
            }
        }
    }

    if (db_ && !explicit_txn) {
        for (u32 i = 0; i < autocommit_record_locks.size(); i++) {
            db_->lock_manager().unlock_record(txn_id, table_id_, autocommit_record_locks[i]);
        }
    }
    if (db_ && count > 0 && !hot_eligible) {
        db_->rebuild_indexes_for_table(table_id_);
    }

    Vector<Value> rv;
    rv.push_back(Value(static_cast<i32>(count)));
    return ExecResult::ok(Tuple(result_schema_, rv));
}

const Schema& UpdateExecutor::output_schema() const { return result_schema_; }

} // namespace minidb
