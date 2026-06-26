/**
 * @file expression_evaluator.h
 * @brief Expression evaluator — evaluate Expression on tuple
 */
#pragma once

#include "record/value.h"
#include "record/tuple.h"
#include "record/schema.h"

namespace minidb {

struct Expression;

class ExpressionEvaluator {
public:
    static Value evaluate(const Expression& expr, const Tuple& tuple);

    // Fast-path: evaluates simple column-op-literal expressions without full AST recursion.
    // Returns true if the fast path was taken (result in `out`).
    // Returns false if the expression is too complex and needs full evaluation.
    static bool fast_evaluate(const Expression& expr, const Tuple& tuple, Value* out);

    // SQL predicate truth extraction. NULL is UNKNOWN and does not pass,
    // while any non-NULL, non-BOOL value is a semantic error for predicate
    // contexts (WHERE/HAVING/JOIN ON/CASE WHEN/AND/OR/NOT).
    static bool predicate_truth(const Value& value, bool* truth);

    // Type-aware ordering of two non-NULL values, matching the semantics used
    // by WHERE/JOIN comparisons: cross-type numerics compare by value and
    // datetime/non-datetime operands are reconciled. Returns <0, 0, >0.
    // Exposed so the compiled predicate evaluator produces identical results
    // to the interpreter (raw Value::compare orders by type-id when types
    // differ, which diverges for e.g. int64-vs-int32 comparisons).
    static int compare_values(const Value& left, const Value& right);
};

} // namespace minidb
