#include "sql/executor/index_lookup_join.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"
#include "storage/heap_file.h"
#include "transaction/transaction.h"

namespace minidb {

IndexLookupJoinExecutor::IndexLookupJoinExecutor(
    UniquePtr<Executor> outer,
    BufferPool* pool,
    HeapFile* inner_heap,
    BPlusTree* inner_index,
    const Schema& inner_storage_schema,
    const Schema& inner_output_schema,
    const Vector<u32>& inner_projected_columns,
    UniquePtr<Expression> on_condition,
    const Schema& output_schema,
    JoinType join_type,
    TransactionManager* txn_mgr)
    : outer_(static_cast<UniquePtr<Executor>&&>(outer)),
      pool_(pool),
      inner_heap_(inner_heap),
      inner_index_(inner_index),
      inner_storage_schema_(inner_storage_schema),
      inner_output_schema_(inner_output_schema),
      inner_projected_columns_(inner_projected_columns),
      on_condition_(static_cast<UniquePtr<Expression>&&>(on_condition)),
      output_schema_(output_schema),
      join_type_(join_type),
      txn_mgr_(txn_mgr),
      outer_key_expr_(nullptr),
      inner_key_expr_(nullptr),
      current_outer_(),
      current_key_(),
      scan_leaf_id_(kNullPageId),
      scan_slot_idx_(0),
      last_index_rid_(),
      has_last_index_rid_(false),
      has_current_outer_(false),
      current_outer_matched_(false) {
    (void)inner_heap_;
}

void IndexLookupJoinExecutor::init() {
    outer_->init();
    setup_keys();
    current_outer_ = Tuple();
    current_key_ = Value();
    scan_leaf_id_ = kNullPageId;
    scan_slot_idx_ = 0;
    last_index_rid_ = RecordId();
    has_last_index_rid_ = false;
    has_current_outer_ = false;
    current_outer_matched_ = false;
}

bool IndexLookupJoinExecutor::can_evaluate_on(Expression* expr, const Schema& schema) const {
    if (!expr) return true;
    switch (expr->type) {
        case ExprType::kLiteral:
        case ExprType::kStar:
            return true;
        case ExprType::kColumnRef:
            if (!expr->table_name.empty()) {
                return schema.get_column_index(expr->table_name, expr->column_name) >= 0;
            }
            return schema.get_column_index(expr->column_name) >= 0;
        case ExprType::kBinaryOp:
            return can_evaluate_on(expr->left.get(), schema) &&
                   can_evaluate_on(expr->right.get(), schema);
        case ExprType::kUnaryOp:
            return can_evaluate_on(expr->child.get(), schema);
        case ExprType::kCase:
        case ExprType::kSubquery:
            return false;
    }
    return false;
}

bool IndexLookupJoinExecutor::setup_keys() {
    if (outer_key_expr_ && inner_key_expr_) return true;
    if (!on_condition_ || on_condition_->type != ExprType::kBinaryOp ||
        on_condition_->op != "=") {
        return false;
    }
    Expression* lhs = on_condition_->left.get();
    Expression* rhs = on_condition_->right.get();
    bool lhs_outer = can_evaluate_on(lhs, outer_->output_schema());
    bool lhs_inner = can_evaluate_on(lhs, inner_output_schema_);
    bool rhs_outer = can_evaluate_on(rhs, outer_->output_schema());
    bool rhs_inner = can_evaluate_on(rhs, inner_output_schema_);

    if (lhs_outer && rhs_inner) {
        outer_key_expr_ = lhs;
        inner_key_expr_ = rhs;
    } else if (rhs_outer && lhs_inner) {
        outer_key_expr_ = rhs;
        inner_key_expr_ = lhs;
    }
    return outer_key_expr_ && inner_key_expr_;
}

Value IndexLookupJoinExecutor::eval_key(Expression* expr, const Tuple& tuple) {
    return ExpressionEvaluator::evaluate(*expr, tuple);
}

bool IndexLookupJoinExecutor::fetch_inner(const RecordId& rid, Tuple* tuple) {
    if (!tuple || !pool_) return false;
    auto result = pool_->fetch_page(rid.page_id);
    if (!result.ok()) return false;
    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(rid.slot_idx);
    SlotIdx visible_slot = rid.slot_idx;
    if (lp && lp->is_redirect()) {
        SlotIdx target = page->redirect_target(rid.slot_idx);
        if (target != kNullSlot) visible_slot = target;
    }
    lp = page->line_pointer(visible_slot);
    if (!lp || !lp->is_valid()) {
        pool_->unpin_page(rid.page_id);
        return false;
    }
    Tuple full = Tuple::deserialize_from_page(page->data() + lp->offset,
                                              inner_storage_schema_, lp->length);
    pool_->unpin_page(rid.page_id);
    if (txn_mgr_ && txn_mgr_->current() &&
        !txn_mgr_->is_visible(full.xmin(), full.xmax(), *txn_mgr_->current())) {
        return false;
    }
    if (!txn_mgr_ && full.xmax() != kInvalidTxnId) return false;
    if (!inner_projected_columns_.empty()) {
        Vector<Value> values;
        for (u32 i = 0; i < inner_projected_columns_.size(); i++) {
            if (inner_projected_columns_[i] < full.column_count()) {
                values.push_back(full.get_value(inner_projected_columns_[i]));
            }
        }
        Tuple projected(inner_output_schema_, values);
        projected.set_xmin(full.xmin());
        projected.set_xmax(full.xmax());
        *tuple = static_cast<Tuple&&>(projected);
    } else {
        Vector<Value> values;
        for (u32 i = 0; i < full.column_count(); i++) values.push_back(full.get_value(i));
        Tuple output(inner_output_schema_, values);
        output.set_xmin(full.xmin());
        output.set_xmax(full.xmax());
        *tuple = static_cast<Tuple&&>(output);
    }
    return true;
}

ExecResult IndexLookupJoinExecutor::make_join_tuple(const Tuple& outer, const Tuple& inner) {
    Vector<Value> values;
    for (u32 i = 0; i < outer.column_count(); i++) values.push_back(outer.get_value(i));
    for (u32 i = 0; i < inner.column_count(); i++) values.push_back(inner.get_value(i));
    return ExecResult::ok(Tuple(output_schema_, values));
}

ExecResult IndexLookupJoinExecutor::make_outer_only_tuple(const Tuple& outer) {
    Vector<Value> values;
    for (u32 i = 0; i < outer.column_count(); i++) values.push_back(outer.get_value(i));
    u32 inner_cols = output_schema_.column_count() > outer.column_count()
        ? output_schema_.column_count() - outer.column_count() : 0;
    for (u32 i = 0; i < inner_cols; i++) values.push_back(Value());
    return ExecResult::ok(Tuple(output_schema_, values));
}

ExecResult IndexLookupJoinExecutor::next() {
    if (!setup_keys() || !inner_index_) return ExecResult::empty();
    while (true) {
        if (executor_cancelled()) return ExecResult::empty();
        if (!has_current_outer_) {
            ExecResult outer = outer_->next();
            if (!outer.ok()) return ExecResult::empty();
            current_outer_ = static_cast<Tuple&&>(outer.tuple);
            current_key_ = eval_key(outer_key_expr_, current_outer_);
            scan_leaf_id_ = kNullPageId;
            scan_slot_idx_ = 0;
            last_index_rid_ = RecordId();
            has_last_index_rid_ = false;
            has_current_outer_ = true;
            current_outer_matched_ = false;
        }

        if (!current_key_.is_null()) {
            while (true) {
                RecordId rid;
                const RecordId* skip = has_last_index_rid_ ? &last_index_rid_ : nullptr;
                if (!inner_index_->scan_next(current_key_, current_key_, &scan_leaf_id_,
                                             &scan_slot_idx_, skip, &rid)) {
                    break;
                }
                last_index_rid_ = rid;
                has_last_index_rid_ = true;
                Tuple inner;
                if (!fetch_inner(rid, &inner)) continue;
                Value inner_key = eval_key(inner_key_expr_, inner);
                if (!inner_key.is_null() && inner_key == current_key_) {
                    current_outer_matched_ = true;
                    return make_join_tuple(current_outer_, inner);
                }
            }
        }

        if (join_type_ == JoinType::kLeft && !current_outer_matched_) {
            Tuple outer = current_outer_;
            has_current_outer_ = false;
            return make_outer_only_tuple(outer);
        }
        has_current_outer_ = false;
    }
}

bool IndexLookupJoinExecutor::fast_count(u64* count) {
    if (!count || !setup_keys() || !inner_index_) return false;
    u64 total = 0;
    while (true) {
        if (executor_cancelled()) return false;
        ExecResult outer = outer_->next();
        if (!outer.ok()) break;
        Value key = eval_key(outer_key_expr_, outer.tuple);
        bool matched = false;
        if (!key.is_null()) {
            PageId leaf = kNullPageId;
            u16 slot = 0;
            RecordId last;
            bool has_last = false;
            while (true) {
                RecordId rid;
                const RecordId* skip = has_last ? &last : nullptr;
                if (!inner_index_->scan_next(key, key, &leaf, &slot, skip, &rid)) break;
                last = rid;
                has_last = true;
                Tuple inner;
                if (!fetch_inner(rid, &inner)) continue;
                Value inner_key = eval_key(inner_key_expr_, inner);
                if (!inner_key.is_null() && inner_key == key) {
                    total++;
                    matched = true;
                }
            }
        }
        if (join_type_ == JoinType::kLeft && !matched) total++;
    }
    *count = total;
    return !executor_error();
}

const Schema& IndexLookupJoinExecutor::output_schema() const { return output_schema_; }

} // namespace minidb
