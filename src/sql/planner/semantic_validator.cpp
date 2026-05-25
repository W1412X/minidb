#include "sql/planner/semantic_validator.h"

namespace minidb {

static bool is_numeric_type(TypeId type) {
    return type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble;
}

static bool is_aggregate_numeric_type(TypeId type) {
    return type == TypeId::kBool || is_numeric_type(type);
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

static bool expressions_equivalent(const Expression* a, const Expression* b) {
    if (a == nullptr || b == nullptr) return a == b;
    if (a->type != b->type) return false;
    if (a->type == ExprType::kLiteral) return a->literal_value == b->literal_value;
    if (a->type == ExprType::kColumnRef) {
        return a->table_name == b->table_name && a->column_name == b->column_name;
    }
    if (a->type == ExprType::kBinaryOp) {
        return a->op == b->op &&
               expressions_equivalent(a->left.get(), b->left.get()) &&
               expressions_equivalent(a->right.get(), b->right.get());
    }
    if (a->type == ExprType::kUnaryOp) {
        return a->op == b->op &&
               expressions_equivalent(a->child.get(), b->child.get());
    }
    if (a->type == ExprType::kCast) {
        return a->cast_target == b->cast_target &&
               expressions_equivalent(a->child.get(), b->child.get());
    }
    if (a->type == ExprType::kCase) {
        if (a->when_clauses.size() != b->when_clauses.size()) return false;
        for (u32 i = 0; i < a->when_clauses.size(); i++) {
            if (!expressions_equivalent(a->when_clauses[i].first.get(),
                                        b->when_clauses[i].first.get()) ||
                !expressions_equivalent(a->when_clauses[i].second.get(),
                                        b->when_clauses[i].second.get())) {
                return false;
            }
        }
        return expressions_equivalent(a->else_expr.get(), b->else_expr.get());
    }
    return false;
}

static bool expression_is_group_key(
    const Expression* expr, const Vector<UniquePtr<Expression>>& group_by) {
    for (u32 i = 0; i < group_by.size(); i++) {
        if (expressions_equivalent(expr, group_by[i].get())) return true;
    }
    return false;
}

static bool aggregate_expression_is_group_legal(
    const Expression* expr, const Vector<UniquePtr<Expression>>& group_by,
    bool inside_aggregate) {
    if (!expr) return false;
    if (!inside_aggregate && expression_is_group_key(expr, group_by)) return true;
    switch (expr->type) {
        case ExprType::kLiteral:
        case ExprType::kSubquery:
            return true;
        case ExprType::kStar:
            return inside_aggregate;
        case ExprType::kColumnRef:
            return inside_aggregate;
        case ExprType::kCast:
            return aggregate_expression_is_group_legal(expr->child.get(), group_by,
                                                       inside_aggregate);
        case ExprType::kUnaryOp:
            return aggregate_expression_is_group_legal(expr->child.get(), group_by,
                                                       inside_aggregate);
        case ExprType::kCase:
            for (u32 i = 0; i < expr->when_clauses.size(); i++) {
                if (!aggregate_expression_is_group_legal(expr->when_clauses[i].first.get(),
                                                         group_by, inside_aggregate) ||
                    !aggregate_expression_is_group_legal(expr->when_clauses[i].second.get(),
                                                         group_by, inside_aggregate)) {
                    return false;
                }
            }
            return !expr->else_expr ||
                   aggregate_expression_is_group_legal(expr->else_expr.get(), group_by,
                                                       inside_aggregate);
        case ExprType::kBinaryOp:
            if (is_aggregate_op(expr->op)) {
                if (inside_aggregate) return false;
                if (!expr->left || expr->left->type == ExprType::kStar) return true;
                return aggregate_expression_is_group_legal(expr->left.get(), group_by, true);
            }
            return aggregate_expression_is_group_legal(expr->left.get(), group_by,
                                                       inside_aggregate) &&
                   aggregate_expression_is_group_legal(expr->right.get(), group_by,
                                                       inside_aggregate);
    }
    return false;
}

bool SemanticValidator::require_bool_expression(const Expression* expr,
                                                const Schema& scope) {
    TypeId type = TypeId::kNull;
    if (!infer_expression_type(expr, scope, &type)) return false;
    return type == TypeId::kBool || type == TypeId::kNull;
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

bool SemanticValidator::validate_aggregate_expression(
    const Expression* expr, const Schema& input_scope,
    const Vector<UniquePtr<Expression>>& group_by) {
    if (!validate_expression(expr, input_scope)) return false;
    return aggregate_expression_is_group_legal(expr, group_by, false);
}

bool SemanticValidator::require_bool_aggregate_expression(
    const Expression* expr, const Schema& input_scope,
    const Vector<UniquePtr<Expression>>& group_by) {
    if (!require_bool_expression(expr, input_scope)) return false;
    return aggregate_expression_is_group_legal(expr, group_by, false);
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
            if (arg_type != TypeId::kNull && !is_aggregate_numeric_type(arg_type)) return false;
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
