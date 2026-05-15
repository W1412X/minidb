/**
 * @file subquery_in_executor.h
 * @brief Subquery IN executor — x IN (SELECT col FROM t2)
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "container/vector.h"
#include "record/value.h"

namespace minidb {

struct Expression;

class SubqueryInExecutor : public Executor {
public:
    SubqueryInExecutor(UniquePtr<Executor> child,
                       UniquePtr<Executor> subquery_exec,
                       UniquePtr<Expression> left_expr,
                       u32 right_col_idx,
                       const Schema& output_schema,
                       bool negate = false);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    bool in_set(const Value& v) const;

    UniquePtr<Executor> child_;
    UniquePtr<Executor> subquery_exec_;
    UniquePtr<Expression> left_expr_;
    u32 right_col_idx_;
    Schema output_schema_;
    Vector<Value> subquery_values_;
    bool subquery_materialized_;
    bool not_;
    bool subquery_has_nulls_;
};

} // namespace minidb
