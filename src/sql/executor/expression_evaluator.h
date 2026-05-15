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
};

} // namespace minidb
