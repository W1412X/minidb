#include "sql/planner/semantic_validator.h"

namespace minidb {

static bool is_numeric_type(TypeId type) {
    return type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble;
}

static bool is_comparison_op(const String& op) {
    return op == "=" || op == "<>" || op == "!=" || op == "<" ||
           op == ">" || op == "<=" || op == ">=" || op == "LIKE";
}

static bool is_arithmetic_op(const String& op) {
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
}

static bool is_aggregate_op(const String& name) {
    return name == "COUNT" || name == "SUM" || name == "AVG" ||
           name == "MIN" || name == "MAX" ||
           name == "COUNT_DISTINCT" || name == "SUM_DISTINCT" ||
           name == "AVG_DISTINCT" || name == "MIN_DISTINCT" ||
           name == "MAX_DISTINCT";
}

static int resolve_column_index_strict(const Schema& schema, const String& table,
                                       const String& column) {
    int found = -1;
    for (u32 i = 0; i < schema.column_count(); i++) {
        const Column& col = schema.get_column(i);
        if (col.is_dropped || col.name != column) continue;
        if (!table.empty()) {
            if (col.table_name == table) return static_cast<int>(i);
            continue;
        }
        if (found >= 0) return -1;  // ambiguous unqualified column
        found = static_cast<int>(i);
    }
    return found;
}

static TypeId numeric_result_type(TypeId left, TypeId right) {
    if (left == TypeId::kDouble || right == TypeId::kDouble) return TypeId::kDouble;
    if (left == TypeId::kFloat || right == TypeId::kFloat) return TypeId::kFloat;
    if (left == TypeId::kInt64 || right == TypeId::kInt64) return TypeId::kInt64;
    return TypeId::kInt32;
}

bool SemanticValidator::require_bool_expression(const Expression* expr,
                                                const Schema& scope) {
    TypeId type = TypeId::kNull;
    if (!infer_expression_type(expr, scope, &type)) return false;
    return type == TypeId::kBool;
}

bool SemanticValidator::validate_expression(const Expression* expr,
                                            const Schema& scope) {
    TypeId ignored = TypeId::kNull;
    return infer_expression_type(expr, scope, &ignored);
}

bool SemanticValidator::validate_order_expression(
    const Expression* expr, const Schema& scope,
    const Vector<UniquePtr<Expression>>& select_list) {
    if (validate_expression(expr, scope)) return true;
    if (!expr || expr->type != ExprType::kColumnRef || !expr->table_name.empty()) {
        return false;
    }
    for (u32 i = 0; i < select_list.size(); i++) {
        if (!select_list[i]->alias.empty() && select_list[i]->alias == expr->column_name) {
            return true;
        }
    }
    return false;
}

static bool infer_aggregate_type(const Expression* expr, const Schema& scope,
                                 TypeId* out) {
    if (!expr) return false;
    if (expr->left && expr->left->type != ExprType::kStar) {
        TypeId arg_type = TypeId::kNull;
        if (!SemanticValidator::infer_expression_type(expr->left.get(), scope, &arg_type)) {
            return false;
        }
        if (expr->op == "SUM" || expr->op == "SUM_DISTINCT" ||
            expr->op == "AVG" || expr->op == "AVG_DISTINCT") {
            if (arg_type != TypeId::kNull && !is_numeric_type(arg_type)) return false;
        }
        if (expr->op == "AVG" || expr->op == "AVG_DISTINCT") {
            if (out) *out = TypeId::kDouble;
        } else if (expr->op == "SUM" || expr->op == "SUM_DISTINCT") {
            if (out) *out = (arg_type == TypeId::kFloat || arg_type == TypeId::kDouble)
                ? arg_type : TypeId::kInt64;
        } else if (expr->op == "MIN" || expr->op == "MIN_DISTINCT" ||
                   expr->op == "MAX" || expr->op == "MAX_DISTINCT") {
            if (out) *out = arg_type;
        } else {
            if (out) *out = TypeId::kInt64;
        }
        return true;
    }
    if (out) *out = TypeId::kInt64;
    return true;
}

bool SemanticValidator::infer_expression_type(const Expression* expr,
                                              const Schema& scope,
                                              TypeId* out) {
    if (!expr) return false;
    switch (expr->type) {
        case ExprType::kLiteral:
            if (out) *out = expr->literal_value.type_id();
            return true;

        case ExprType::kColumnRef: {
            int idx = resolve_column_index_strict(scope, expr->table_name, expr->column_name);
            if (idx < 0) return false;
            if (out) *out = scope.get_column(static_cast<u32>(idx)).type;
            return true;
        }

        case ExprType::kStar:
            if (out) *out = TypeId::kNull;
            return true;

        case ExprType::kSubquery:
            // Scalar subquery typing is handled by its own planner path. Treat
            // it as unknown here, but keep surrounding expressions bound.
            if (out) *out = TypeId::kNull;
            return true;

        case ExprType::kCast:
            if (!validate_expression(expr->child.get(), scope)) return false;
            if (out) *out = expr->cast_target;
            return true;

        case ExprType::kCase: {
            TypeId result_type = TypeId::kNull;
            for (u32 i = 0; i < expr->when_clauses.size(); i++) {
                if (!require_bool_expression(expr->when_clauses[i].first.get(), scope)) {
                    return false;
                }
                TypeId branch_type = TypeId::kNull;
                if (!infer_expression_type(expr->when_clauses[i].second.get(), scope,
                                           &branch_type)) {
                    return false;
                }
                if (result_type == TypeId::kNull) result_type = branch_type;
            }
            if (expr->else_expr) {
                TypeId else_type = TypeId::kNull;
                if (!infer_expression_type(expr->else_expr.get(), scope, &else_type)) {
                    return false;
                }
                if (result_type == TypeId::kNull) result_type = else_type;
            }
            if (out) *out = result_type;
            return true;
        }

        case ExprType::kUnaryOp: {
            if (expr->op == "NOT") {
                if (!require_bool_expression(expr->child.get(), scope)) return false;
                if (out) *out = TypeId::kBool;
                return true;
            }
            TypeId child_type = TypeId::kNull;
            if (!infer_expression_type(expr->child.get(), scope, &child_type)) return false;
            if (expr->op == "IS_NULL" || expr->op == "IS_NOT_NULL") {
                if (out) *out = TypeId::kBool;
                return true;
            }
            if (expr->op == "LENGTH") {
                if (child_type != TypeId::kNull && child_type != TypeId::kVarchar) return false;
                if (out) *out = TypeId::kInt32;
                return true;
            }
            if (expr->op == "-") {
                if (child_type != TypeId::kNull && !is_numeric_type(child_type)) return false;
                if (out) *out = child_type;
                return true;
            }
            return false;
        }

        case ExprType::kBinaryOp: {
            if (is_aggregate_op(expr->op)) {
                return infer_aggregate_type(expr, scope, out);
            }
            if (expr->op == "AND" || expr->op == "OR") {
                if (!require_bool_expression(expr->left.get(), scope) ||
                    !require_bool_expression(expr->right.get(), scope)) {
                    return false;
                }
                if (out) *out = TypeId::kBool;
                return true;
            }
            if (is_comparison_op(expr->op)) {
                if (!validate_expression(expr->left.get(), scope) ||
                    !validate_expression(expr->right.get(), scope)) {
                    return false;
                }
                if (out) *out = TypeId::kBool;
                return true;
            }
            if (expr->op == "IN_SUBQUERY" || expr->op == "NOT_IN_SUBQUERY") {
                if (!validate_expression(expr->left.get(), scope)) return false;
                if (out) *out = TypeId::kBool;
                return true;
            }
            if (expr->op == "COALESCE" || expr->op == "NULLIF") {
                TypeId left_type = TypeId::kNull;
                if (!infer_expression_type(expr->left.get(), scope, &left_type) ||
                    !validate_expression(expr->right.get(), scope)) {
                    return false;
                }
                if (out) *out = left_type;
                return true;
            }
            if (is_arithmetic_op(expr->op)) {
                TypeId left_type = TypeId::kNull;
                TypeId right_type = TypeId::kNull;
                if (!infer_expression_type(expr->left.get(), scope, &left_type) ||
                    !infer_expression_type(expr->right.get(), scope, &right_type)) {
                    return false;
                }
                if (left_type != TypeId::kNull && !is_numeric_type(left_type)) return false;
                if (right_type != TypeId::kNull && !is_numeric_type(right_type)) return false;
                if (out) *out = numeric_result_type(left_type, right_type);
                return true;
            }
            return false;
        }
    }
    return false;
}

} // namespace minidb
