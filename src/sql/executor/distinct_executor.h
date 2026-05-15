/**
 * @file distinct_executor.h / .cpp
 * @brief DISTINCT executor — 物化去重
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "container/vector.h"

namespace minidb {

class DistinctExecutor : public Executor {
public:
    DistinctExecutor(UniquePtr<Executor> child, const Schema& output_schema,
                     u64 work_mem_bytes = 0);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    bool tuples_equal(const Tuple& a, const Tuple& b) const;

    UniquePtr<Executor> child_;
    Schema output_schema_;
    Vector<Tuple> seen_;
    u32 cursor_;
    u64 work_mem_bytes_;
};

} // namespace minidb
