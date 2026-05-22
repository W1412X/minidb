/**
 * @file filter.h / .cpp
 * @brief Filter Executor — applies a WHERE predicate over a child iterator.
 *
 * The compiled stack-machine that evaluates the predicate lives in
 * CompiledPredicate so the same engine is reused for filter pushdown
 * inside scan operators.
 */
#pragma once

#include "sql/executor/executor.h"
#include "sql/executor/compiled_predicate.h"
#include "container/unique_ptr.h"
#include "record/schema.h"
#include "record/value.h"

namespace minidb {

struct Expression;

class FilterExecutor : public Executor {
public:
    FilterExecutor(UniquePtr<Executor> child, UniquePtr<Expression> predicate);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;
    bool last_record_id(RecordId* rid) const override;

    Executor* child() const { return child_.get(); }

private:
    UniquePtr<Executor> child_;
    UniquePtr<Expression> predicate_;
    CompiledPredicate compiled_;
};

} // namespace minidb
