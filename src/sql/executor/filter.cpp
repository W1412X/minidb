#include "sql/executor/filter.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"

namespace minidb {

FilterExecutor::FilterExecutor(UniquePtr<Executor> child, UniquePtr<Expression> predicate)
    : child_(static_cast<UniquePtr<Executor>&&>(child)),
      predicate_(static_cast<UniquePtr<Expression>&&>(predicate)) {}

void FilterExecutor::init() {
    child_->init();
    compiled_.compile(predicate_.get(), child_->output_schema());
}

ExecResult FilterExecutor::next() {
    while (true) {
        ExecResult result = child_->next();
        if (!result.ok()) return result;
        if (compiled_.compiled()) {
            if (compiled_.passes(result.tuple)) return result;
            continue;
        }
        // Compile failed (unsupported expression shape such as subqueries
        // or function calls). Fall back to the recursive AST evaluator.
        Value cond;
        if (!ExpressionEvaluator::fast_evaluate(*predicate_, result.tuple, &cond)) {
            cond = ExpressionEvaluator::evaluate(*predicate_, result.tuple);
        }
        if (!cond.is_null() && cond.get_bool()) return result;
    }
}

const Schema& FilterExecutor::output_schema() const { return child_->output_schema(); }

bool FilterExecutor::last_record_id(RecordId* rid) const {
    return child_->last_record_id(rid);
}

} // namespace minidb
