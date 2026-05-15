/**
 * @file limit_executor.h
 * @brief LIMIT/OFFSET Executor
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"

namespace minidb {

class LimitExecutor : public Executor {
public:
    LimitExecutor(UniquePtr<Executor> child, i32 limit, i32 offset);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    UniquePtr<Executor> child_;
    i32 limit_;
    i32 offset_;
    i32 rows_returned_;
    i32 rows_skipped_;
    bool done_;
};

} // namespace minidb
