/**
 * @file planner.cpp
 * @brief Query planner — supports JOIN, DISTINCT, ORDER BY, LIMIT/OFFSET, aggregation, UNION, SHOW, DESC, CREATE INDEX
 */
#include "sql/planner/planner.h"
#include "sql/optimizer/optimizer.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/planner/semantic_validator.h"
#include <cstdio>

namespace minidb {

static String int_to_string(int val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    return String(buf);
}

static Schema schema_with_table_name(const Schema& schema, const String& table_name) {
    Schema out;
    for (u32 i = 0; i < schema.column_count(); i++) {
        Column col = schema.get_column(i);
        col.table_name = table_name;
        out.add_column(col);
    }
    return out;
}

static Schema schema_with_alias_if_present(const Schema& schema, const String& alias) {
    if (alias.empty()) return schema;
    return schema_with_table_name(schema, alias);
}

Planner::Planner(Catalog* catalog) : catalog_(catalog), optimizer_config_() {}

Planner::Planner(Catalog* catalog, const OptimizerConfig& optimizer_config)
    : catalog_(catalog), optimizer_config_(optimizer_config) {}

static UniquePtr<PlanNode> optimize_plan(Catalog* catalog, const OptimizerConfig& config,
                                         UniquePtr<PlanNode> plan) {
    Optimizer optimizer(catalog, config);
    return optimizer.optimize(static_cast<UniquePtr<PlanNode>&&>(plan));
}

UniquePtr<PlanNode> Planner::plan(const Statement& stmt) {
    switch (stmt.type) {
        case StmtType::kSelect:
            return stmt.select ? optimize_plan(catalog_, optimizer_config_, plan_select(*stmt.select)) : UniquePtr<PlanNode>();
        case StmtType::kInsert:
            return stmt.insert ? optimize_plan(catalog_, optimizer_config_, plan_insert(*stmt.insert)) : UniquePtr<PlanNode>();
        case StmtType::kUpdate:
            return stmt.update ? optimize_plan(catalog_, optimizer_config_, plan_update(*stmt.update)) : UniquePtr<PlanNode>();
        case StmtType::kDelete:
            return stmt.delete_stmt ? optimize_plan(catalog_, optimizer_config_, plan_delete(*stmt.delete_stmt)) : UniquePtr<PlanNode>();
        case StmtType::kCreateTable:
            return stmt.create_table ? plan_create_table(*stmt.create_table) : UniquePtr<PlanNode>();
        case StmtType::kCreateIndex:
            return stmt.create_index ? plan_create_index(*stmt.create_index) : UniquePtr<PlanNode>();
        case StmtType::kDescTable:
            return stmt.desc_table ? plan_desc_table(*stmt.desc_table) : UniquePtr<PlanNode>();
        case StmtType::kShowTables: {
            auto plan = make_unique<SeqScanPlan>();
            plan->table_id = 0;  // sentinel: SHOW TABLES handled directly in server/REPL
            plan->table_name = "";
            plan->output_schema = Schema();
            return UniquePtr<PlanNode>(plan.release());
        }
        case StmtType::kDropTable:
        default: return UniquePtr<PlanNode>();
    }
}

bool Planner::is_aggregate_func(const String& name) {
    return name == "COUNT" || name == "SUM" || name == "AVG" ||
           name == "MIN" || name == "MAX" ||
           name == "COUNT_DISTINCT" || name == "SUM_DISTINCT" ||
           name == "AVG_DISTINCT" || name == "MIN_DISTINCT" ||
           name == "MAX_DISTINCT";
}

static bool is_aggregate_op(const String& name) {
    return name == "COUNT" || name == "SUM" || name == "AVG" ||
           name == "MIN" || name == "MAX" ||
           name == "COUNT_DISTINCT" || name == "SUM_DISTINCT" ||
           name == "AVG_DISTINCT" || name == "MIN_DISTINCT" ||
           name == "MAX_DISTINCT";
}

static bool exprs_equivalent(const Expression* a, const Expression* b) {
    if (a == nullptr || b == nullptr) return a == b;
    if (a->type != b->type) return false;
    if (a->type == ExprType::kLiteral) return a->literal_value == b->literal_value;
    if (a->type == ExprType::kColumnRef) {
        return a->table_name == b->table_name && a->column_name == b->column_name;
    }
    if (a->type == ExprType::kBinaryOp) {
        return a->op == b->op &&
               exprs_equivalent(a->left.get(), b->left.get()) &&
               exprs_equivalent(a->right.get(), b->right.get());
    }
    if (a->type == ExprType::kUnaryOp) {
        return a->op == b->op && exprs_equivalent(a->child.get(), b->child.get());
    }
    if (a->type == ExprType::kCase) {
        if (a->when_clauses.size() != b->when_clauses.size()) return false;
        for (u32 i = 0; i < a->when_clauses.size(); i++) {
            if (!exprs_equivalent(a->when_clauses[i].first.get(), b->when_clauses[i].first.get()) ||
                !exprs_equivalent(a->when_clauses[i].second.get(), b->when_clauses[i].second.get())) {
                return false;
            }
        }
        return exprs_equivalent(a->else_expr.get(), b->else_expr.get());
    }
    if (a->type == ExprType::kCast) {
        return a->cast_target == b->cast_target &&
               exprs_equivalent(a->child.get(), b->child.get());
    }
    return false;
}

static int find_aggregate_expr_index(const Expression* expr,
                                     const Vector<UniquePtr<Expression>>& aggregate_exprs) {
    for (u32 i = 0; i < aggregate_exprs.size(); i++) {
        if (exprs_equivalent(expr, aggregate_exprs[i].get())) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void collect_aggregate_exprs(const Expression* expr,
                                    const Vector<UniquePtr<Expression>>& existing,
                                    Vector<UniquePtr<Expression>>& out) {
    if (!expr) return;
    if (expr->type == ExprType::kBinaryOp &&
        is_aggregate_op(expr->op)) {
        if (find_aggregate_expr_index(expr, existing) < 0 &&
            find_aggregate_expr_index(expr, out) < 0) {
            out.push_back(UniquePtr<Expression>(expr->clone()));
        }
        return;
    }
    collect_aggregate_exprs(expr->left.get(), existing, out);
    collect_aggregate_exprs(expr->right.get(), existing, out);
    collect_aggregate_exprs(expr->child.get(), existing, out);
    for (u32 i = 0; i < expr->when_clauses.size(); i++) {
        collect_aggregate_exprs(expr->when_clauses[i].first.get(), existing, out);
        collect_aggregate_exprs(expr->when_clauses[i].second.get(), existing, out);
    }
    collect_aggregate_exprs(expr->else_expr.get(), existing, out);
}

static UniquePtr<Expression> rewrite_having_expr(
    const Expression* expr,
    const Vector<UniquePtr<Expression>>& aggregate_exprs,
    const Vector<String>& aggregate_column_names) {
    if (!expr) return UniquePtr<Expression>();
    if (expr->type == ExprType::kBinaryOp &&
        is_aggregate_op(expr->op)) {
        int idx = find_aggregate_expr_index(expr, aggregate_exprs);
        if (idx >= 0) {
            auto col = make_unique<Expression>();
            col->type = ExprType::kColumnRef;
            col->column_name = aggregate_column_names[static_cast<u32>(idx)];
            return UniquePtr<Expression>(col.release());
        }
    }

    auto copy = make_unique<Expression>();
    copy->type = expr->type;
    copy->table_name = expr->table_name;
    copy->column_name = expr->column_name;
    copy->literal_value = expr->literal_value;
    copy->alias = expr->alias;
    copy->op = expr->op;
    if (expr->left) copy->left = rewrite_having_expr(expr->left.get(), aggregate_exprs, aggregate_column_names);
    if (expr->right) copy->right = rewrite_having_expr(expr->right.get(), aggregate_exprs, aggregate_column_names);
    if (expr->child) copy->child = rewrite_having_expr(expr->child.get(), aggregate_exprs, aggregate_column_names);
    for (u32 i = 0; i < expr->when_clauses.size(); i++) {
        Pair<UniquePtr<Expression>, UniquePtr<Expression>> clause;
        clause.first = rewrite_having_expr(expr->when_clauses[i].first.get(), aggregate_exprs, aggregate_column_names);
        clause.second = rewrite_having_expr(expr->when_clauses[i].second.get(), aggregate_exprs, aggregate_column_names);
        copy->when_clauses.push_back(static_cast<Pair<UniquePtr<Expression>, UniquePtr<Expression>>&&>(clause));
    }
    if (expr->else_expr) {
        copy->else_expr = rewrite_having_expr(expr->else_expr.get(), aggregate_exprs, aggregate_column_names);
    }
    return UniquePtr<Expression>(copy.release());
}

bool Planner::has_aggregate(const Vector<UniquePtr<Expression>>& list) {
    for (u32 i = 0; i < list.size(); i++) {
        Vector<UniquePtr<Expression>> found;
        collect_aggregate_exprs(list[i].get(), found, found);
        if (!found.empty())
            return true;
    }
    return false;
}

UniquePtr<PlanNode> Planner::plan_select(const SelectStmt& stmt) {
    UniquePtr<PlanNode> left = plan_select_core(stmt, !stmt.union_rhs);
    if (!left) return UniquePtr<PlanNode>();
    if (!stmt.union_rhs) return left;

    UniquePtr<PlanNode> right = plan_select(*stmt.union_rhs);
    if (!right) return UniquePtr<PlanNode>();

    auto plan = make_unique<UnionPlan>();
    plan->left = UniquePtr<PlanNode>(left.release());
    plan->right = UniquePtr<PlanNode>(right.release());
    plan->all = stmt.union_all;
    plan->output_schema = plan->left->output_schema;
    UniquePtr<PlanNode> current(plan.release());
    Schema union_schema = current->output_schema;

    if (!stmt.order_by.empty()) {
        auto sort = make_unique<SortPlan>();
        if (stmt.limit >= 0) sort->top_n = stmt.limit + (stmt.offset > 0 ? stmt.offset : 0);
        sort->child = UniquePtr<PlanNode>(current.release());
        for (u32 i = 0; i < stmt.order_by.size(); i++) {
            if (!SemanticValidator::validate_order_expression(stmt.order_by[i].expression.get(),
                                           union_schema, stmt.select_list)) {
                return UniquePtr<PlanNode>();
            }
            SortKey sk;
            sk.expression = UniquePtr<Expression>(stmt.order_by[i].expression->clone());
            sk.ascending = stmt.order_by[i].ascending;
            sk.nulls_first = stmt.order_by[i].nulls_first;
            sort->keys.push_back(static_cast<SortKey&&>(sk));
        }
        sort->output_schema = union_schema;
        current = UniquePtr<PlanNode>(sort.release());
    }

    if (stmt.limit >= 0 || stmt.offset >= 0) {
        auto lim = make_unique<LimitPlan>();
        lim->child = UniquePtr<PlanNode>(current.release());
        lim->limit = stmt.limit;
        lim->offset = stmt.offset;
        lim->output_schema = lim->child->output_schema;
        current = UniquePtr<PlanNode>(lim.release());
    }

    return current;
}

UniquePtr<PlanNode> Planner::plan_select_core(const SelectStmt& stmt, bool include_tail) {
    Schema merged_schema;
    bool where_satisfied_by_index = false;
    UniquePtr<PlanNode> current;

    if (stmt.from_tables.empty()) {
        if (!stmt.joins.empty()) return UniquePtr<PlanNode>();
        auto one = make_unique<OneRowPlan>();
        one->output_schema = merged_schema;
        current = UniquePtr<PlanNode>(one.release());
    } else {
        const TableRef& from = stmt.from_tables[0];
        if (from.subquery) {
            current = plan_select(*from.subquery);
            if (!current) return UniquePtr<PlanNode>();
            merged_schema = schema_with_alias_if_present(current->output_schema, from.alias);
            current->output_schema = merged_schema;
        } else {
            TableEntry* table = catalog_->get_table(from.name);
            if (!table) return UniquePtr<PlanNode>();

            String left_alias = from.alias.empty()
                                ? table->table_name : from.alias;

            merged_schema = schema_with_table_name(table->schema, left_alias);

            current = plan_table_access(
                table, merged_schema, stmt.joins.empty() ? stmt.where_clause.get() : nullptr,
                &where_satisfied_by_index);
        }
    }

    // JOIN
    for (u32 j = 0; j < stmt.joins.size(); j++) {
        const JoinClause& join = stmt.joins[j];
        UniquePtr<PlanNode> right_plan;
        Schema right_schema;
        if (join.table.subquery) {
            right_plan = plan_select(*join.table.subquery);
            if (!right_plan) return UniquePtr<PlanNode>();
            right_schema = schema_with_alias_if_present(right_plan->output_schema, join.table.alias);
            right_plan->output_schema = right_schema;
        } else {
            TableEntry* right_table = catalog_->get_table(join.table.name);
            if (!right_table) return UniquePtr<PlanNode>();
            String right_alias = join.table.alias.empty() ? right_table->table_name : join.table.alias;

            auto right_scan = make_unique<SeqScanPlan>();
            right_scan->table_id = right_table->table_id;
            right_scan->table_name = right_table->table_name;
            right_scan->output_schema = schema_with_table_name(right_table->schema, right_alias);
            right_schema = right_scan->output_schema;
            right_plan = UniquePtr<PlanNode>(right_scan.release());
        }

        Schema join_schema = merged_schema;
        for (u32 i = 0; i < right_schema.column_count(); i++) {
            join_schema.add_column(right_schema.get_column(i));
        }
        if (join.on_condition &&
            !SemanticValidator::require_bool_expression(join.on_condition.get(), join_schema)) {
            return UniquePtr<PlanNode>();
        }

        auto join_plan = make_unique<JoinPlan>();
        join_plan->left = UniquePtr<PlanNode>(current.release());
        join_plan->right = UniquePtr<PlanNode>(right_plan.release());
        join_plan->on_condition = join.on_condition
            ? UniquePtr<Expression>(join.on_condition->clone())
            : UniquePtr<Expression>();
        join_plan->join_type = join.type;

        merged_schema = join_schema;
        join_plan->output_schema = merged_schema;
        current = UniquePtr<PlanNode>(join_plan.release());
    }

    // Filter (WHERE) — emit as a FilterPlan; the optimizer's
    // `optimize_filter` rewrites it into IndexScan when an index covers
    // the predicate, and ultimately pushes residual conjuncts directly
    // into the underlying scan via `pushed_predicate` so that no separate
    // Filter operator survives in the executor pipeline.
    if (stmt.where_clause && !where_satisfied_by_index) {
        if (!SemanticValidator::require_bool_expression(stmt.where_clause.get(), merged_schema)) {
            return UniquePtr<PlanNode>();
        }
        auto filter = make_unique<FilterPlan>();
        filter->child = UniquePtr<PlanNode>(current.release());
        filter->predicate = UniquePtr<Expression>(stmt.where_clause->clone());
        filter->output_schema = merged_schema;
        current = UniquePtr<PlanNode>(filter.release());
    } else if (stmt.where_clause &&
               !SemanticValidator::require_bool_expression(stmt.where_clause.get(), merged_schema)) {
        return UniquePtr<PlanNode>();
    }

    // Aggregation (GROUP BY / HAVING / COUNT/SUM/AVG/MIN/MAX)
    if (has_aggregate(stmt.select_list) || !stmt.group_by.empty()) {
        for (u32 i = 0; i < stmt.select_list.size(); i++) {
            if (!SemanticValidator::validate_expression(stmt.select_list[i].get(), merged_schema)) {
                return UniquePtr<PlanNode>();
            }
        }
        for (u32 i = 0; i < stmt.group_by.size(); i++) {
            if (!SemanticValidator::validate_expression(stmt.group_by[i].get(), merged_schema)) {
                return UniquePtr<PlanNode>();
            }
        }
        if (stmt.having && !SemanticValidator::require_bool_expression(stmt.having.get(), merged_schema)) {
            return UniquePtr<PlanNode>();
        }

        auto agg = make_unique<AggregatePlan>();
        agg->child = UniquePtr<PlanNode>(current.release());

        Schema agg_out;
        Vector<UniquePtr<Expression>> aggregate_exprs;
        for (u32 i = 0; i < stmt.select_list.size(); i++) {
            collect_aggregate_exprs(stmt.select_list[i].get(), aggregate_exprs, aggregate_exprs);
        }
        if (stmt.having) {
            collect_aggregate_exprs(stmt.having.get(), aggregate_exprs, aggregate_exprs);
        }

        for (u32 i = 0; i < stmt.group_by.size(); i++) {
            Column col;
            if (stmt.group_by[i]->type == ExprType::kColumnRef) {
                int idx = -1;
                if (!stmt.group_by[i]->table_name.empty()) {
                    idx = merged_schema.get_column_index(stmt.group_by[i]->table_name,
                                                         stmt.group_by[i]->column_name);
                } else {
                    idx = merged_schema.get_column_index(stmt.group_by[i]->column_name);
                }
                if (idx >= 0) {
                    col = merged_schema.get_column(static_cast<u32>(idx));
                } else {
                    col.name = String("group_") + int_to_string(i);
                    col.type = TypeId::kInt64;
                }
            } else {
                col.name = String("group_") + int_to_string(i);
                col.type = TypeId::kInt64;
            }
            agg_out.add_column(col);
            agg->group_by.push_back(UniquePtr<Expression>(stmt.group_by[i]->clone()));
        }

        Vector<String> aggregate_column_names;
        for (u32 i = 0; i < aggregate_exprs.size(); i++) {
            const auto& expr = aggregate_exprs[i];
            AggregateColumn ac;
            if (expr->op == "COUNT" || expr->op == "COUNT_DISTINCT") ac.func = AggFunc::kCount;
            else if (expr->op == "SUM" || expr->op == "SUM_DISTINCT") ac.func = AggFunc::kSum;
            else if (expr->op == "AVG" || expr->op == "AVG_DISTINCT") ac.func = AggFunc::kAvg;
            else if (expr->op == "MIN" || expr->op == "MIN_DISTINCT") ac.func = AggFunc::kMin;
            else if (expr->op == "MAX" || expr->op == "MAX_DISTINCT") ac.func = AggFunc::kMax;
            ac.distinct = expr->op == "COUNT_DISTINCT" || expr->op == "SUM_DISTINCT" ||
                          expr->op == "AVG_DISTINCT" || expr->op == "MIN_DISTINCT" ||
                          expr->op == "MAX_DISTINCT";
            // COUNT(*) has no argument; Executor pushes 1 per row
            if (expr->left && expr->left->type != ExprType::kStar) {
                ac.argument = UniquePtr<Expression>(expr->left->clone());
            }
            Column col;
            col.name = String("agg_") + int_to_string(i);
            aggregate_column_names.push_back(col.name);
            // Type inference
            if (ac.func == AggFunc::kAvg) {
                col.type = TypeId::kDouble;
            } else if (ac.func == AggFunc::kCount) {
                col.type = TypeId::kInt64;
            } else if (ac.argument && ac.argument->type == ExprType::kColumnRef) {
                int arg_idx = merged_schema.get_column_index(ac.argument->column_name);
                if (arg_idx >= 0) {
                    TypeId arg_type = merged_schema.get_column(static_cast<u32>(arg_idx)).type;
                    col.type = (ac.func == AggFunc::kSum || ac.func == AggFunc::kAvg)
                               ? (arg_type == TypeId::kFloat || arg_type == TypeId::kDouble
                                  ? arg_type : TypeId::kInt64)
                               : arg_type;
                } else {
                    col.type = TypeId::kInt64;
                }
            } else {
                col.type = TypeId::kInt64;
            }
            agg_out.add_column(col);
            agg->aggregates.push_back(static_cast<AggregateColumn&&>(ac));
        }

        if (stmt.having) {
            agg->having = rewrite_having_expr(stmt.having.get(), aggregate_exprs, aggregate_column_names);
        }
        agg->output_schema = agg_out;
        current = UniquePtr<PlanNode>(agg.release());
        merged_schema = agg_out;

        auto proj = make_unique<ProjectPlan>();
        proj->child = UniquePtr<PlanNode>(current.release());
        Schema out_schema;
        for (u32 i = 0; i < stmt.select_list.size(); i++) {
            const auto& expr = stmt.select_list[i];
            if (expr->type == ExprType::kBinaryOp && is_aggregate_func(expr->op)) {
                int agg_idx = find_aggregate_expr_index(expr.get(), aggregate_exprs);
                if (agg_idx < 0) return UniquePtr<PlanNode>();
                u32 output_idx = stmt.group_by.size() + static_cast<u32>(agg_idx);
                proj->column_indices.push_back(output_idx);
                Column out_col = merged_schema.get_column(output_idx);
                if (!expr->alias.empty()) out_col.name = expr->alias;
                out_schema.add_column(out_col);
                continue;
            }

            if (expr->type == ExprType::kColumnRef) {
                int group_idx = -1;
                for (u32 g = 0; g < stmt.group_by.size(); g++) {
                    const auto& group_expr = stmt.group_by[g];
                    if (group_expr->type != ExprType::kColumnRef) continue;
                    bool same_table = (expr->table_name.empty() || group_expr->table_name.empty() ||
                                       expr->table_name == group_expr->table_name);
                    if (same_table && expr->column_name == group_expr->column_name) {
                        group_idx = static_cast<int>(g);
                        break;
                    }
                }
                if (group_idx < 0) return UniquePtr<PlanNode>();
                proj->column_indices.push_back(static_cast<u32>(group_idx));
                Column out_col = merged_schema.get_column(static_cast<u32>(group_idx));
                if (!expr->alias.empty()) out_col.name = expr->alias;
                out_schema.add_column(out_col);
                continue;
            }

            auto rewritten = rewrite_having_expr(expr.get(), aggregate_exprs, aggregate_column_names);
            if (!rewritten) return UniquePtr<PlanNode>();
            proj->expressions.push_back(static_cast<UniquePtr<Expression>&&>(rewritten));
            Column col;
            col.name = expr->alias.empty() ? String("expr_") + int_to_string(i) : expr->alias;
            col.type = TypeId::kInt64;
            out_schema.add_column(col);
        }
        proj->output_schema = out_schema;
        current = UniquePtr<PlanNode>(proj.release());
        merged_schema = out_schema;
    }

    // Project (SELECT list)
    bool is_star = false;
    if (stmt.select_list.size() == 1) {
        is_star = (stmt.select_list[0]->type == ExprType::kStar);
    }

    bool order_applied_before_projection = false;
    if (include_tail && !stmt.order_by.empty() && !stmt.distinct &&
        !has_aggregate(stmt.select_list)) {
        for (u32 i = 0; i < stmt.order_by.size(); i++) {
            if (!SemanticValidator::validate_order_expression(stmt.order_by[i].expression.get(),
                                           merged_schema, stmt.select_list)) {
                return UniquePtr<PlanNode>();
            }
        }
        auto sort = make_unique<SortPlan>();
        if (stmt.limit >= 0) sort->top_n = stmt.limit + (stmt.offset > 0 ? stmt.offset : 0);
        sort->child = UniquePtr<PlanNode>(current.release());
        for (u32 i = 0; i < stmt.order_by.size(); i++) {
            SortKey sk;
            sk.expression = UniquePtr<Expression>(stmt.order_by[i].expression->clone());
            sk.ascending = stmt.order_by[i].ascending;
            sk.nulls_first = stmt.order_by[i].nulls_first;
            sort->keys.push_back(static_cast<SortKey&&>(sk));
        }
        sort->output_schema = merged_schema;
        current = UniquePtr<PlanNode>(sort.release());
        order_applied_before_projection = true;
    }

    // SELECT * with dropped columns: expand into an explicit projection
    // that skips the invisible columns. This keeps the rest of the planner
    // and executor unaware of dropped columns — they only see visible ones.
    // Note: is_star stays true so the existing non-star projection block
    // (which re-processes stmt.select_list) is correctly skipped.
    if (is_star && merged_schema.has_dropped_columns()) {
        Vector<UniquePtr<Expression>> exprs;
        Schema out_schema;
        for (u32 i = 0; i < merged_schema.column_count(); i++) {
            const Column& col = merged_schema.get_column(i);
            if (col.is_dropped) continue;
            auto e = make_unique<Expression>();
            e->type = ExprType::kColumnRef;
            e->column_name = col.name;
            e->table_name = col.table_name;
            exprs.push_back(UniquePtr<Expression>(e.release()));
            out_schema.add_column(col);
        }
        if (!exprs.empty()) {
            auto proj = make_unique<ProjectPlan>();
            proj->child = UniquePtr<PlanNode>(current.release());
            proj->expressions = static_cast<Vector<UniquePtr<Expression>>&&>(exprs);
            proj->output_schema = out_schema;
            current = UniquePtr<PlanNode>(proj.release());
            merged_schema = out_schema;
        }
    }

    if (!is_star && !has_aggregate(stmt.select_list)) {
        Vector<UniquePtr<Expression>> exprs;
        Schema out_schema;
        for (u32 i = 0; i < stmt.select_list.size(); i++) {
            const auto& expr = stmt.select_list[i];
            if (!SemanticValidator::validate_expression(expr.get(), merged_schema)) {
                return UniquePtr<PlanNode>();
            }
            if (expr->type == ExprType::kColumnRef) {
                int idx = -1;
                if (!expr->table_name.empty()) {
                    idx = merged_schema.get_column_index(expr->table_name, expr->column_name);
                } else {
                    idx = merged_schema.get_column_index(expr->column_name);
                }
                if (idx < 0) {
                    // Column not found (or dropped) — reject the query.
                    return UniquePtr<PlanNode>();
                }
                exprs.push_back(UniquePtr<Expression>(expr->clone()));
                Column out_col = merged_schema.get_column(static_cast<u32>(idx));
                if (!expr->alias.empty()) out_col.name = expr->alias;
                out_schema.add_column(out_col);
            } else {
                // Table expression projection (a+1, b*2, function calls, etc.)
                exprs.push_back(UniquePtr<Expression>(expr->clone()));
                Column col;
                col.name = expr->alias.empty() ? String("expr_") + int_to_string(i) : expr->alias;
                col.type = TypeId::kInt64;
                out_schema.add_column(col);
            }
        }
        if (!exprs.empty()) {
            auto proj = make_unique<ProjectPlan>();
            proj->child = UniquePtr<PlanNode>(current.release());
            proj->expressions = static_cast<Vector<UniquePtr<Expression>>&&>(exprs);
            proj->output_schema = out_schema;
            current = UniquePtr<PlanNode>(proj.release());
            merged_schema = out_schema;
        }
    }

    // DISTINCT applies to the projected row, not the source tuple.
    if (stmt.distinct) {
        auto dist = make_unique<DistinctPlan>();
        dist->child = UniquePtr<PlanNode>(current.release());
        dist->output_schema = merged_schema;
        current = UniquePtr<PlanNode>(dist.release());
    }

    // ORDER BY
    if (include_tail && !stmt.order_by.empty() && !order_applied_before_projection) {
        for (u32 i = 0; i < stmt.order_by.size(); i++) {
            if (!SemanticValidator::validate_order_expression(stmt.order_by[i].expression.get(),
                                           merged_schema, stmt.select_list)) {
                return UniquePtr<PlanNode>();
            }
        }
        auto sort = make_unique<SortPlan>();
        if (stmt.limit >= 0) sort->top_n = stmt.limit + (stmt.offset > 0 ? stmt.offset : 0);
        sort->child = UniquePtr<PlanNode>(current.release());
        for (u32 i = 0; i < stmt.order_by.size(); i++) {
            SortKey sk;
            sk.expression = UniquePtr<Expression>(stmt.order_by[i].expression->clone());
            sk.ascending = stmt.order_by[i].ascending;
            sk.nulls_first = stmt.order_by[i].nulls_first;
            sort->keys.push_back(static_cast<SortKey&&>(sk));
        }
        sort->output_schema = merged_schema;
        current = UniquePtr<PlanNode>(sort.release());
    }

    // LIMIT / OFFSET
    if (include_tail && (stmt.limit >= 0 || stmt.offset >= 0)) {
        auto lim = make_unique<LimitPlan>();
        lim->child = UniquePtr<PlanNode>(current.release());
        lim->limit = stmt.limit;
        lim->offset = stmt.offset;
        lim->output_schema = merged_schema;
        current = UniquePtr<PlanNode>(lim.release());
    }

    return current;
}

UniquePtr<PlanNode> Planner::plan_insert(const InsertStmt& stmt) {
    TableEntry* table = catalog_->get_table(stmt.table.name);
    if (!table) return UniquePtr<PlanNode>();
    auto plan = make_unique<InsertPlan>();
    plan->table_id = table->table_id;
    plan->output_schema = table->schema;

    Vector<u32> target_columns;
    if (stmt.columns.empty()) {
        // Implicit column list: only visible (non-dropped) columns.
        // Dropped columns keep their physical slot but are invisible to
        // the user — new rows store NULL in those positions.
        for (u32 i = 0; i < table->schema.column_count(); i++) {
            if (!table->schema.get_column(i).is_dropped) {
                target_columns.push_back(i);
            }
        }
    } else {
        for (u32 i = 0; i < stmt.columns.size(); i++) {
            int idx = table->schema.get_column_index(stmt.columns[i]);
            if (idx < 0) return UniquePtr<PlanNode>();
            target_columns.push_back(static_cast<u32>(idx));
        }
    }

    // Columns omitted from an explicit column list pick up their declared
    // DEFAULT (or NULL when none was declared). The NOT NULL check at
    // execution time then rejects rows where a NOT NULL column has neither
    // an explicit value nor a default.
    Vector<bool> column_supplied;
    column_supplied.resize(table->schema.column_count());
    for (u32 i = 0; i < target_columns.size(); i++) {
        column_supplied[target_columns[i]] = true;
    }

    for (u32 r = 0; r < stmt.values_list.size(); r++) {
        if (stmt.values_list[r].size() != target_columns.size()) {
            return UniquePtr<PlanNode>();
        }

        Vector<Value> row;
        row.resize(table->schema.column_count());
        for (u32 i = 0; i < table->schema.column_count(); i++) {
            if (!column_supplied[i]) row[i] = table->schema.get_column(i).default_as_value();
        }
        for (u32 c = 0; c < stmt.values_list[r].size(); c++) {
            const auto& expr = stmt.values_list[r][c];
            Value v;
            if (expr->type == ExprType::kLiteral) {
                v = expr->literal_value;
            } else if (expr->type == ExprType::kSubquery && expr->subquery &&
                       expr->subquery->from_tables.empty() &&
                       expr->subquery->select_list.size() == 1) {
                Tuple dummy;
                v = ExpressionEvaluator::evaluate(*expr->subquery->select_list[0], dummy);
            } else {
                // Evaluate non-literal expressions (e.g., 1+1, COALESCE(a, 0))
                Tuple dummy;
                v = ExpressionEvaluator::evaluate(*expr, dummy);
            }
            row[target_columns[c]] = v;
        }
        plan->values.push_back(Vector<Value>(row));
    }
    return UniquePtr<PlanNode>(plan.release());
}

UniquePtr<PlanNode> Planner::plan_delete(const DeleteStmt& stmt) {
    TableEntry* table = catalog_->get_table(stmt.table.name);
    if (!table) return UniquePtr<PlanNode>();
    auto plan = make_unique<DeletePlan>();
    plan->table_id = table->table_id;
    plan->output_schema = table->schema;
    Schema access_schema = schema_with_table_name(
        table->schema, stmt.table.alias.empty() ? table->table_name : stmt.table.alias);
    if (stmt.where_clause && !SemanticValidator::require_bool_expression(stmt.where_clause.get(), access_schema)) {
        return UniquePtr<PlanNode>();
    }
    bool where_satisfied_by_index = false;
    UniquePtr<PlanNode> child = plan_table_access(
        table, access_schema, stmt.where_clause.get(), &where_satisfied_by_index);
    if (stmt.where_clause && !where_satisfied_by_index) {
        auto filter = make_unique<FilterPlan>();
        filter->child = UniquePtr<PlanNode>(child.release());
        filter->predicate = UniquePtr<Expression>(stmt.where_clause->clone());
        filter->output_schema = access_schema;
        child = UniquePtr<PlanNode>(filter.release());
    }
    plan->child = UniquePtr<PlanNode>(child.release());
    return UniquePtr<PlanNode>(plan.release());
}

UniquePtr<PlanNode> Planner::plan_update(const UpdateStmt& stmt) {
    TableEntry* table = catalog_->get_table(stmt.table.name);
    if (!table) return UniquePtr<PlanNode>();
    auto plan = make_unique<UpdatePlan>();
    plan->table_id = table->table_id;
    plan->output_schema = table->schema;
    Schema access_schema = schema_with_table_name(
        table->schema, stmt.table.alias.empty() ? table->table_name : stmt.table.alias);
    for (u32 i = 0; i < stmt.set_clauses.size(); i++) {
        if (table->schema.get_column_index(stmt.set_clauses[i].first) < 0) {
            return UniquePtr<PlanNode>();
        }
        if (!SemanticValidator::validate_expression(stmt.set_clauses[i].second.get(), table->schema)) {
            return UniquePtr<PlanNode>();
        }
        Pair<String, UniquePtr<Expression>> clause;
        clause.first = stmt.set_clauses[i].first;
        clause.second = UniquePtr<Expression>(stmt.set_clauses[i].second->clone());
        plan->set_clauses.push_back(static_cast<Pair<String, UniquePtr<Expression>>&&>(clause));
    }
    if (stmt.where_clause && !SemanticValidator::require_bool_expression(stmt.where_clause.get(), access_schema)) {
        return UniquePtr<PlanNode>();
    }
    bool where_satisfied_by_index = false;
    UniquePtr<PlanNode> child = plan_table_access(
        table, access_schema, stmt.where_clause.get(), &where_satisfied_by_index);
    if (stmt.where_clause) {
        plan->where_clause = UniquePtr<Expression>(stmt.where_clause->clone());
    }
    if (stmt.where_clause && !where_satisfied_by_index) {
        auto filter = make_unique<FilterPlan>();
        filter->child = UniquePtr<PlanNode>(child.release());
        filter->predicate = UniquePtr<Expression>(stmt.where_clause->clone());
        filter->output_schema = access_schema;
        child = UniquePtr<PlanNode>(filter.release());
    }
    plan->child = UniquePtr<PlanNode>(child.release());
    return UniquePtr<PlanNode>(plan.release());
}

UniquePtr<PlanNode> Planner::plan_create_table(const CreateTableStmt& stmt) {
    (void)stmt;
    return UniquePtr<PlanNode>();
}

UniquePtr<PlanNode> Planner::plan_table_access(TableEntry* table, const Schema& visible_schema,
                                               const Expression* predicate,
                                               bool* predicate_satisfied) {
    (void)predicate;
    if (predicate_satisfied) *predicate_satisfied = false;
    if (!table) return UniquePtr<PlanNode>();

    auto scan = make_unique<SeqScanPlan>();
    scan->table_id = table->table_id;
    scan->table_name = table->table_name;
    scan->output_schema = visible_schema;
    return UniquePtr<PlanNode>(scan.release());
}

UniquePtr<PlanNode> Planner::plan_create_index(const CreateIndexStmt& stmt) {
    (void)stmt;
    return UniquePtr<PlanNode>();
}

UniquePtr<PlanNode> Planner::plan_desc_table(const DescTableStmt& stmt) {
    TableEntry* table = catalog_->get_table(stmt.table_name);
    if (!table) return UniquePtr<PlanNode>();
    // Return an empty plan node carrying only schema info for executor output
    auto plan = make_unique<SeqScanPlan>();
    plan->table_id = table->table_id;
    plan->table_name = table->table_name;
    plan->output_schema = table->schema;
    return UniquePtr<PlanNode>(plan.release());
}

} // namespace minidb
