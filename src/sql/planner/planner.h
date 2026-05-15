/**
 * @file planner.h / .cpp
 * @brief Query planner — Convert AST to logical plan tree
 */
#pragma once

#include "common/defs.h"
#include "catalog/catalog.h"
#include "sql/parser/ast.h"
#include "sql/planner/plan_node.h"
#include "sql/optimizer/optimizer.h"
#include "container/unique_ptr.h"

namespace minidb {

class Planner {
public:
    explicit Planner(Catalog* catalog);
    Planner(Catalog* catalog, const OptimizerConfig& optimizer_config);

    UniquePtr<PlanNode> plan(const Statement& stmt);

private:
    UniquePtr<PlanNode> plan_select(const SelectStmt& stmt);
    UniquePtr<PlanNode> plan_select_core(const SelectStmt& stmt, bool include_tail = true);
    UniquePtr<PlanNode> plan_insert(const InsertStmt& stmt);
    UniquePtr<PlanNode> plan_update(const UpdateStmt& stmt);
    UniquePtr<PlanNode> plan_delete(const DeleteStmt& stmt);
    UniquePtr<PlanNode> plan_create_table(const CreateTableStmt& stmt);
    UniquePtr<PlanNode> plan_create_index(const CreateIndexStmt& stmt);
    UniquePtr<PlanNode> plan_desc_table(const DescTableStmt& stmt);
    UniquePtr<PlanNode> plan_table_access(TableEntry* table, const Schema& visible_schema,
                                          const Expression* predicate,
                                          bool* predicate_satisfied);

    bool has_aggregate(const Vector<UniquePtr<Expression>>& list);
    bool is_aggregate_func(const String& name);

    Catalog* catalog_;
    OptimizerConfig optimizer_config_;
};

} // namespace minidb
