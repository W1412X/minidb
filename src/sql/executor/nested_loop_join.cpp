/**
 * @file nested_loop_join.cpp
 * @brief Nested loop join executor implementation — supports INNER JOIN and LEFT JOIN
 */
#include "sql/executor/nested_loop_join.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"

namespace minidb {

NestedLoopJoinExecutor::NestedLoopJoinExecutor(
    UniquePtr<Executor> left, UniquePtr<Executor> right,
    UniquePtr<Expression> on_condition, const Schema& output_schema,
    JoinType join_type)
    : left_(static_cast<UniquePtr<Executor>&&>(left)),
      right_(static_cast<UniquePtr<Executor>&&>(right)),
      on_condition_(static_cast<UniquePtr<Expression>&&>(on_condition)),
      output_schema_(output_schema), join_type_(join_type),
      left_exhausted_(false), right_matched_(false) {}

void NestedLoopJoinExecutor::init() {
    left_->init();
    right_->init();
    left_tuple_ = Tuple();
    left_exhausted_ = false;
    right_matched_ = false;
}

ExecResult NestedLoopJoinExecutor::next() {
    while (true) {
        // Need to fetch new left tuple
        if (!left_exhausted_ && left_tuple_.column_count() == 0) {
            ExecResult lr = left_->next();
            if (!lr.ok()) {
                left_exhausted_ = true;
                return ExecResult::empty();
            }
            left_tuple_ = static_cast<Tuple&&>(lr.tuple);
            right_->init();
            right_matched_ = false;
        }

        // Attempt to match right tuple
        ExecResult rr = right_->next();
        if (rr.ok()) {
            if (on_condition_) {
                Vector<Value> combined_values;
                for (u32 i = 0; i < left_tuple_.column_count(); i++) {
                    combined_values.push_back(left_tuple_.get_value(i));
                }
                for (u32 i = 0; i < rr.tuple.column_count(); i++) {
                    combined_values.push_back(rr.tuple.get_value(i));
                }

                Tuple combined(output_schema_, combined_values);
                Value cond = ExpressionEvaluator::evaluate(*on_condition_, combined);
                bool pass = false;
                if (!ExpressionEvaluator::predicate_truth(cond, &pass)) {
                    set_executor_error("JOIN ON expression must be BOOL");
                    return ExecResult::empty();
                }
                if (pass) {
                    right_matched_ = true;
                    return make_combined_tuple(left_tuple_, rr.tuple);
                }
            } else {
                right_matched_ = true;
                return make_combined_tuple(left_tuple_, rr.tuple);
            }
            continue;
        }

        // Right table exhausted
        if (join_type_ == JoinType::kLeft && !right_matched_) {
            // LEFT JOIN: no matching rows, output left tuple + right side NULL
            Tuple left_only = left_tuple_;
            left_tuple_ = Tuple();
            return make_left_only_tuple(left_only);
        }

        // Next left tuple
        left_tuple_ = Tuple();
    }
}

ExecResult NestedLoopJoinExecutor::make_combined_tuple(const Tuple& left, const Tuple& right) {
    Vector<Value> values;
    values.reserve(left.column_count() + right.column_count());
    for (u32 i = 0; i < left.column_count(); i++) {
        values.push_back(left.get_value(i));
    }
    for (u32 i = 0; i < right.column_count(); i++) {
        values.push_back(right.get_value(i));
    }
    return ExecResult::ok(Tuple(output_schema_, values));
}

ExecResult NestedLoopJoinExecutor::make_left_only_tuple(const Tuple& left) {
    Vector<Value> values;
    values.reserve(output_schema_.column_count());
    for (u32 i = 0; i < left.column_count(); i++) {
        values.push_back(left.get_value(i));
    }
    // Right columns fill with NULL
    u32 out_cols = output_schema_.column_count();
    u32 left_cols = left.column_count();
    if (out_cols > left_cols) {
        for (u32 i = 0; i < out_cols - left_cols; i++) {
            values.push_back(Value());
        }
    }
    return ExecResult::ok(Tuple(output_schema_, values));
}

const Schema& NestedLoopJoinExecutor::output_schema() const { return output_schema_; }

} // namespace minidb
