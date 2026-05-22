/**
 * @file compiled_predicate.h
 * @brief Reusable compiled-predicate engine shared by Filter and pushed-down
 *        scan predicates.
 *
 * Compiles a SQL Expression AST into a flat stack-machine program once at
 * init() time. Evaluating against a tuple is then a single linear pass over
 * the program with a pre-allocated value stack — no recursion, no per-row
 * heap allocation. This is the inner loop of every WHERE clause.
 *
 * Design notes
 * ------------
 *   - The shape mirrors FilterExecutor::CompiledNode exactly so its existing
 *     test coverage still pins the compiler semantics.
 *   - Three-valued logic (TRUE / FALSE / NULL) is preserved: an unevaluable
 *     comparison yields a NULL Value, and `passes()` treats NULL as failure
 *     (the standard WHERE semantics — NULL rows are excluded).
 *   - `passes()` is the only hot-path entry point; it avoids returning by
 *     value to keep things branch-predictor-friendly.
 *   - Owned by SeqScanExecutor / IndexScanExecutor / FilterExecutor; one
 *     instance per scan, reused for the lifetime of a single statement.
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include "container/vector.h"
#include "record/tuple.h"
#include "record/value.h"

#include <vector>

namespace minidb {

struct Expression;

class CompiledPredicate {
public:
    CompiledPredicate() = default;

    // Build the program from `expr`, resolving column references against
    // `schema`. Returns true if every leaf could be resolved. A `false`
    // return means the caller must fall back to AST evaluation.
    bool compile(const Expression* expr, const Schema& schema);

    // Evaluate the predicate against `tuple`. Returns true iff the predicate
    // is TRUE — false for FALSE and for SQL NULL (standard WHERE semantics).
    bool passes(const Tuple& tuple) const;

    // Returns true iff compile() produced a usable program. When false the
    // caller should fall back to the AST-walking evaluator.
    bool compiled() const { return root_ >= 0; }

private:
    struct Node {
        enum class Kind : u8 {
            kLiteral, kColumn, kCompare,
            kAnd, kOr, kNot, kIsNull, kIsNotNull
        };
        Kind kind;
        u8 op_code = 0;     // for Compare: 0 ==, 1 !=, 2 <, 3 >, 4 <=, 5 >=
        u32 column_idx = 0;
        i32 left = -1;
        i32 right = -1;
        Value literal;
    };

    i32 compile_node(const Expression* expr, const Schema& schema);

    Vector<Node> nodes_;
    i32 root_ = -1;
    // Reused across evaluations; resized once in compile().
    mutable std::vector<Value> eval_stack_;
};

} // namespace minidb
