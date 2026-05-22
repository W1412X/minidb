/**
 * @file filter.h / .cpp
 * @brief Filter Executor
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "record/schema.h"
#include "record/value.h"
#include <vector>

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
    struct CompiledNode {
        enum class Kind { kLiteral, kColumn, kCompare, kAnd, kOr, kNot, kIsNull, kIsNotNull };
        Kind kind;
        Value literal;
        u32 column_idx = 0;
        String op;
        int left = -1;
        int right = -1;
    };

    int compile_expr(const Expression* expr, const Schema& schema);
    bool eval_compiled(const Tuple& tuple, Value* out) const;

    UniquePtr<Executor> child_;
    UniquePtr<Expression> predicate_;
    std::vector<CompiledNode> compiled_nodes_;
    int compiled_root_ = -1;
    // Pre-allocated evaluation stack reused across every `next()` call.
    // The old code re-allocated `std::vector<Value>(N)` on every row,
    // which dominated CPU time in selective scans.
    mutable std::vector<Value> eval_stack_;
};

} // namespace minidb
