/**
 * @file expression_evaluator.cpp
 * @brief ExpressionEvaluator — SQL 3-valued logic
 */
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"
#include "container/string.h"
#include <limits>

namespace minidb {

// NULL helpers
static bool is_null(const Value& v) { return v.is_null(); }

static bool is_numeric_like(TypeId type) {
    return type == TypeId::kBool || type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble;
}

static double numeric_as_double(const Value& value) {
    switch (value.type_id()) {
        case TypeId::kBool: return value.get_bool() ? 1.0 : 0.0;
        case TypeId::kInt32: return static_cast<double>(value.get_int32());
        case TypeId::kInt64: return static_cast<double>(value.get_int64());
        case TypeId::kFloat: return static_cast<double>(value.get_float());
        case TypeId::kDouble: return value.get_double();
        default: return 0.0;
    }
}

static int sql_compare_values(const Value& left, const Value& right) {
    if (is_numeric_like(left.type_id()) && is_numeric_like(right.type_id())) {
        double l = numeric_as_double(left);
        double r = numeric_as_double(right);
        return (l < r) ? -1 : (l > r ? 1 : 0);
    }
    return left.compare(right);
}

static Value compare_result(const Value& left, const Value& right, const String& op) {
    int cmp = sql_compare_values(left, right);
    if (op == "=") return Value(cmp == 0);
    if (op == "<>" || op == "!=") return Value(cmp != 0);
    if (op == "<") return Value(cmp < 0);
    if (op == ">") return Value(cmp > 0);
    if (op == "<=") return Value(cmp <= 0);
    if (op == ">=") return Value(cmp >= 0);
    return Value();
}

// LIKE pattern matching: % = any character sequence, _ = single character
static bool like_match(const char* str, u32 str_len, const char* pat, u32 pat_len) {
    u32 si = 0, pi = 0;
    u32 star_si = 0, star_pi = 0;
    bool star = false;

    while (si < str_len) {
        if (pi < pat_len && pat[pi] == '%') {
            star = true;
            star_si = si;
            star_pi = pi;
            pi++;
        } else if (pi < pat_len && (pat[pi] == str[si] || pat[pi] == '_')) {
            si++;
            pi++;
        } else if (star) {
            pi = star_pi + 1;
            star_si++;
            si = star_si;
        } else {
            return false;
        }
    }

    while (pi < pat_len && pat[pi] == '%') pi++;
    return pi == pat_len;
}

bool ExpressionEvaluator::fast_evaluate(const Expression& expr, const Tuple& tuple, Value* out) {
    // Handle simple predicates: col = literal, col > literal, col < literal, etc.
    if (expr.type == ExprType::kBinaryOp && expr.left && expr.left->type == ExprType::kColumnRef &&
        expr.right && expr.right->type == ExprType::kLiteral && !expr.op.empty()) {
        int idx = -1;
        if (!expr.left->table_name.empty()) {
            idx = tuple.schema().get_column_index(expr.left->table_name, expr.left->column_name);
        } else {
            idx = tuple.schema().get_column_index(expr.left->column_name);
        }
        if (idx >= 0) {
            Value col_val = tuple.get_value(static_cast<u32>(idx));
            const Value& lit_val = expr.right->literal_value;
            if (col_val.is_null()) {
                // All comparison operators return NULL on NULL input
                *out = Value();
                return true;
            }
            const String& op = expr.op;
            if (op == "=" || op == "<>" || op == "!=" || op == "<" ||
                op == ">" || op == "<=" || op == ">=") {
                *out = compare_result(col_val, lit_val, op);
                return true;
            }
        }
        return false;
    }

    // Handle literal = col (reversed)
    if (expr.type == ExprType::kBinaryOp && expr.left && expr.left->type == ExprType::kLiteral &&
        expr.right && expr.right->type == ExprType::kColumnRef && !expr.op.empty()) {
        int idx = -1;
        if (!expr.right->table_name.empty()) {
            idx = tuple.schema().get_column_index(expr.right->table_name, expr.right->column_name);
        } else {
            idx = tuple.schema().get_column_index(expr.right->column_name);
        }
        if (idx >= 0) {
            Value col_val = tuple.get_value(static_cast<u32>(idx));
            const Value& lit_val = expr.left->literal_value;
            if (col_val.is_null()) {
                *out = Value();
                return true;
            }
            const String& op = expr.op;
            if (op == "=" || op == "<>" || op == "!=") {
                *out = compare_result(col_val, lit_val, op);
                return true;
            }
            if (op == "<")  { *out = compare_result(col_val, lit_val, ">"); return true; }
            if (op == ">")  { *out = compare_result(col_val, lit_val, "<"); return true; }
            if (op == "<=") { *out = compare_result(col_val, lit_val, ">="); return true; }
            if (op == ">=") { *out = compare_result(col_val, lit_val, "<="); return true; }
        }
        return false;
    }

    // Handle column IS NULL / IS NOT NULL
    if (expr.type == ExprType::kUnaryOp && expr.child &&
        expr.child->type == ExprType::kColumnRef &&
        (expr.op == "IS_NULL" || expr.op == "IS_NOT_NULL")) {
        int idx = -1;
        if (!expr.child->table_name.empty()) {
            idx = tuple.schema().get_column_index(expr.child->table_name, expr.child->column_name);
        } else {
            idx = tuple.schema().get_column_index(expr.child->column_name);
        }
        if (idx >= 0) {
            Value col_val = tuple.get_value(static_cast<u32>(idx));
            *out = Value(expr.op == "IS_NULL" ? col_val.is_null() : !col_val.is_null());
            return true;
        }
        return false;
    }

    return false;
}

Value ExpressionEvaluator::evaluate(const Expression& expr, const Tuple& tuple) {
    switch (expr.type) {
        case ExprType::kLiteral:
            return expr.literal_value;

        case ExprType::kColumnRef: {
            int idx = -1;
            if (!expr.table_name.empty()) {
                idx = tuple.schema().get_column_index(expr.table_name, expr.column_name);
            } else {
                idx = tuple.schema().get_column_index(expr.column_name);
            }
            if (idx < 0) return Value();
            return tuple.get_value(static_cast<u32>(idx));
        }

        case ExprType::kBinaryOp: {
            if (!expr.left) return Value();
            Value left = evaluate(*expr.left, tuple);
            Value right = expr.right ? evaluate(*expr.right, tuple) : Value();
            const String& op = expr.op;

            // SQL NULL propagation: comparison with NULL yields NULL.
            if (op == "=" || op == "<>" || op == "!=" || op == "<" || op == ">" ||
                op == "<=" || op == ">=") {
                if (is_null(left) || is_null(right)) return Value();
                return compare_result(left, right, op);
            }

            // AND: SQL three-valued logic.
            if (op == "AND") {
                if (is_null(left) || is_null(right)) {
                    if (!is_null(left) && !left.get_bool()) return Value(false);
                    if (!is_null(right) && !right.get_bool()) return Value(false);
                    return Value();  // NULL
                }
                return Value(left.get_bool() && right.get_bool());
            }

            // OR: SQL three-valued logic.
            if (op == "OR") {
                if (is_null(left) || is_null(right)) {
                    if (!is_null(left) && left.get_bool()) return Value(true);
                    if (!is_null(right) && right.get_bool()) return Value(true);
                    return Value();  // NULL
                }
                return Value(left.get_bool() || right.get_bool());
            }

            // Arithmetic: NULL propagation.
            if (op == "+") { if (is_null(left) || is_null(right)) return Value(); return left + right; }
            if (op == "-") { if (is_null(left) || is_null(right)) return Value(); return left - right; }
            if (op == "*") { if (is_null(left) || is_null(right)) return Value(); return left * right; }
            if (op == "/") { if (is_null(left) || is_null(right)) return Value(); return left / right; }
            if (op == "%") { if (is_null(left) || is_null(right)) return Value(); return left % right; }

            if (op == "IS_NULL") return Value(is_null(left));
            if (op == "IS_NOT_NULL") return Value(!is_null(left));
            if (op == "LIKE") {
                if (is_null(left) || is_null(right)) return Value();
                const String& str = left.get_string();
                const String& pat = right.get_string();
                return Value(like_match(str.c_str(), str.size(), pat.c_str(), pat.size()));
            }
            if (op == "COALESCE") {
                if (!is_null(left)) return left;
                return right;
            }
            if (op == "NULLIF") {
                if (is_null(left) || is_null(right)) return Value();
                if (left == right) return Value();
                return left;
            }
            if (op == "IN_SUBQUERY") {
                // Already handled by SubqueryInExecutor.
                return Value();
            }
            return Value();
        }

        case ExprType::kUnaryOp: {
            if (!expr.child) return Value();
            Value child = evaluate(*expr.child, tuple);

            if (expr.op == "-") {
                if (is_null(child)) return Value();
                if (child.type_id() == TypeId::kInt32) {
                    if (child.get_int32() == std::numeric_limits<i32>::min()) return Value();
                    return Value(-child.get_int32());
                }
                if (child.type_id() == TypeId::kInt64) {
                    if (child.get_int64() == std::numeric_limits<i64>::min()) return Value();
                    return Value(-child.get_int64());
                }
                if (child.type_id() == TypeId::kFloat) return Value(-child.get_float());
                if (child.type_id() == TypeId::kDouble) return Value(-child.get_double());
            }

            if (expr.op == "NOT") {
                if (is_null(child)) return Value();  // NOT NULL = NULL
                return Value(!child.get_bool());
            }

            if (expr.op == "IS_NULL") {
                return Value(is_null(child));
            }

            if (expr.op == "IS_NOT_NULL") {
                return Value(!is_null(child));
            }

            if (expr.op == "LENGTH") {
                if (is_null(child) || child.type_id() != TypeId::kVarchar) return Value();
                return Value(static_cast<i32>(child.get_string().size()));
            }

            return Value();
        }

        case ExprType::kStar:
            return Value();

        case ExprType::kSubquery:
            return Value();

        case ExprType::kCase: {
            for (u32 i = 0; i < expr.when_clauses.size(); i++) {
                Value cond = evaluate(*expr.when_clauses[i].first, tuple);
                if (!is_null(cond) && cond.get_bool()) {
                    return evaluate(*expr.when_clauses[i].second, tuple);
                }
            }
            if (expr.else_expr) {
                return evaluate(*expr.else_expr, tuple);
            }
            return Value();
        }
    }

    return Value();
}

} // namespace minidb
