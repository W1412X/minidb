#include "sql/executor/project.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"

namespace minidb {

ProjectExecutor::ProjectExecutor(UniquePtr<Executor> child, const Schema& out,
                                 const Vector<u32>& cols,
                                 const Vector<UniquePtr<Expression>>& exprs)
    : child_(static_cast<UniquePtr<Executor>&&>(child)),
      output_schema_(out), col_indices_(cols) {
    for (u32 i = 0; i < exprs.size(); i++) {
        expressions_.push_back(UniquePtr<Expression>(exprs[i]->clone()));
    }
}

void ProjectExecutor::init() { child_->init(); }

ExecResult ProjectExecutor::next() {
    ExecResult result = child_->next();
    if (!result.ok()) return result;

    Vector<Value> new_values;

    // Simple column reference
    for (u32 i = 0; i < col_indices_.size(); i++) {
        if (col_indices_[i] < result.tuple.column_count()) {
            new_values.push_back(result.tuple.get_value(col_indices_[i]));
        } else {
            new_values.push_back(Value());
        }
    }

    // Table expression evaluation
    for (u32 i = 0; i < expressions_.size(); i++) {
        new_values.push_back(ExpressionEvaluator::evaluate(*expressions_[i], result.tuple));
    }

    return ExecResult::ok(Tuple(output_schema_, new_values));
}

const Schema& ProjectExecutor::output_schema() const { return output_schema_; }

bool ProjectExecutor::last_record_id(RecordId* rid) const {
    return child_->last_record_id(rid);
}

} // namespace minidb
