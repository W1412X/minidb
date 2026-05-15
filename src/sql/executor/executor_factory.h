/**
 * @file executor_factory.h / .cpp
 * @brief Executor Factory — convert PlanNode to Executor tree
 */
#pragma once

#include "common/defs.h"
#include "container/unique_ptr.h"
#include "sql/executor/executor.h"
#include "sql/planner/plan_node.h"
#include "database/database.h"

namespace minidb {

struct SelectStmt;

class ExecutorFactory {
public:
    explicit ExecutorFactory(Database& db);

    UniquePtr<Executor> create(PlanNode* plan);

private:
    UniquePtr<Expression> materialize_scalar_subqueries(const Expression* expr);
    Value evaluate_scalar_subquery(const SelectStmt* subquery);

    Database& db_;
};

} // namespace minidb
