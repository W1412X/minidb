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
    static bool infer_expression_type(const Expression* expr, const Schema& scope,
                                      TypeId* out);
};

} // namespace minidb
