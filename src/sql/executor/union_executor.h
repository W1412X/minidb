/**
 * @file union_executor.h / .cpp
 * @brief UNION / UNION ALL Executor
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "container/vector.h"

namespace minidb {

class UnionExecutor : public Executor {
public:
    UnionExecutor(UniquePtr<Executor> left, UniquePtr<Executor> right,
                  bool all, const Schema& output_schema);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    bool tuples_equal(const Tuple& a, const Tuple& b) const;

    UniquePtr<Executor> left_;
    UniquePtr<Executor> right_;
    bool all_;
    Schema output_schema_;
    bool left_done_;
    Vector<Tuple> buffer_;
    u32 cursor_;
};

} // namespace minidb
