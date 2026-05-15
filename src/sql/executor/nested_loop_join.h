/**
 * @file nested_loop_join.h
 * @brief Nested loop join executor
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "record/schema.h"
#include "sql/planner/plan_node.h"

namespace minidb {

struct Expression;

class NestedLoopJoinExecutor : public Executor {
public:
    NestedLoopJoinExecutor(UniquePtr<Executor> left, UniquePtr<Executor> right,
                           UniquePtr<Expression> on_condition, const Schema& output_schema,
                           JoinType join_type = JoinType::kInner);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    ExecResult make_combined_tuple(const Tuple& left, const Tuple& right);
    ExecResult make_left_only_tuple(const Tuple& left);

    UniquePtr<Executor> left_;
    UniquePtr<Executor> right_;
    UniquePtr<Expression> on_condition_;
    Schema output_schema_;
    JoinType join_type_;
    Tuple left_tuple_;
    bool left_exhausted_;
    bool right_matched_;
};

} // namespace minidb
