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
};

} // namespace minidb
