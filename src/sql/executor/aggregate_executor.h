/**
 * @file aggregate_executor.h
 * @brief Aggregate executor — COUNT/SUM/AVG/MIN/MAX + GROUP BY + HAVING
 */
#pragma once

#include "sql/executor/executor.h"
#include "sql/planner/plan_node.h"
#include "container/unique_ptr.h"
#include "container/vector.h"
#include "container/hash_map.h"

namespace minidb {

class AggregateExecutor : public Executor {
public:
    AggregateExecutor(UniquePtr<Executor> child,
                      Vector<AggregateColumn>&& aggregates,
                      Vector<UniquePtr<Expression>>&& group_by,
                      UniquePtr<Expression> having,
                      const Schema& output_schema,
                      u64 work_mem_bytes = 0);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    void compute_groups();
    bool compute_groups_sort_spill();

    UniquePtr<Executor> child_;
    Vector<AggregateColumn> aggregates_;
    Vector<UniquePtr<Expression>> group_by_;
    UniquePtr<Expression> having_;
    Schema output_schema_;
    Vector<Vector<Value>> result_groups_;
    u32 cursor_;
    u64 work_mem_bytes_;
};

} // namespace minidb
