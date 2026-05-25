#include "sql/executor/subquery_in_executor.h"
#include "common/tuple_key.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"

namespace minidb {

SubqueryInExecutor::SubqueryInExecutor(
    UniquePtr<Executor> child, UniquePtr<Executor> subquery_exec,
    UniquePtr<Expression> left_expr, u32 right_col_idx,
    const Schema& output_schema, bool negate)
    : child_(static_cast<UniquePtr<Executor>&&>(child)),
      subquery_exec_(static_cast<UniquePtr<Executor>&&>(subquery_exec)),
      left_expr_(static_cast<UniquePtr<Expression>&&>(left_expr)),
      right_col_idx_(right_col_idx), output_schema_(output_schema),
      subquery_materialized_(false), not_(negate), subquery_has_nulls_(false) {}

void SubqueryInExecutor::init() {
    child_->init();
    subquery_keys_.clear();
    subquery_materialized_ = false;
    subquery_has_nulls_ = false;
}

bool SubqueryInExecutor::in_set(const Value& v) const {
    if (v.is_null()) {
        // NULL = NULL is UNKNOWN in SQL, never matches for equality
        return false;
    }
    String key = encode_value_key(v);
    return subquery_keys_.find(key) != nullptr;
}

ExecResult SubqueryInExecutor::next() {
    // Lazy materialize subquery result
    if (!subquery_materialized_) {
        subquery_exec_->init();
        while (true) {
            ExecResult r = subquery_exec_->next();
            if (!r.ok()) break;
            Value val = r.tuple.get_value(right_col_idx_);
            if (val.is_null()) {
                subquery_has_nulls_ = true;
            } else {
                subquery_keys_.insert(encode_value_key(val), true);
            }
        }
        subquery_materialized_ = true;
    }

    while (true) {
        ExecResult r = child_->next();
        if (!r.ok()) return ExecResult::empty();

        Value left_val = ExpressionEvaluator::evaluate(*left_expr_, r.tuple);
        bool found = in_set(left_val);
        if (not_) {
            // NOT IN: SQL semantics
            // - If subquery contains NULL: result is always NULL (no rows), because x != NULL = UNKNOWN
            // - if the subquery result has no NULL: regular membership check.
            if (subquery_has_nulls_) continue;
            if (!found) return r;
        } else {
            // IN: NULL never matches; only exact non-NULL membership counts.
            if (found) return r;
        }
    }
}

const Schema& SubqueryInExecutor::output_schema() const { return output_schema_; }

} // namespace minidb
