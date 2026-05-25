#pragma once

#include "record/schema.h"
#include "sql/parser/ast.h"

namespace minidb {

class SemanticValidator {
public:
    static bool validate_expression(const Expression* expr, const Schema& scope);
    static bool require_bool_expression(const Expression* expr, const Schema& scope);
    static bool validate_order_expression(
        const Expression* expr, const Schema& scope,
        const Vector<UniquePtr<Expression>>& select_list);
    static bool validate_aggregate_expression(
        const Expression* expr, const Schema& input_scope,
        const Vector<UniquePtr<Expression>>& group_by);
    static bool require_bool_aggregate_expression(
        const Expression* expr, const Schema& input_scope,
        const Vector<UniquePtr<Expression>>& group_by);
    static bool infer_expression_type(const Expression* expr, const Schema& scope,
                                      TypeId* out);
};

} // namespace minidb
