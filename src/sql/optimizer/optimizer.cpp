#include "sql/optimizer/optimizer.h"
#include "sql/parser/ast.h"
#include "common/tuple_key.h"

namespace minidb {

// Walk an expression AST looking for a subquery node. Used to decide whether
// a Filter predicate is safe to push down into a scan operator — predicates
// that reference subqueries need the FilterPlan node so the ExecutorFactory
// can wire the inner plan (SubqueryInExecutor, scalar-subquery materialisation).
static bool expr_contains_subquery(const Expression* expr) {
    if (!expr) return false;
    if (expr->type == ExprType::kSubquery) return true;
    if (expr->type == ExprType::kBinaryOp &&
        (expr->op == "IN_SUBQUERY" || expr->op == "NOT_IN_SUBQUERY" ||
         expr->op == "EXISTS" || expr->op == "NOT_EXISTS")) {
        return true;
    }
    if (expr->left && expr_contains_subquery(expr->left.get())) return true;
    if (expr->right && expr_contains_subquery(expr->right.get())) return true;
    if (expr->child && expr_contains_subquery(expr->child.get())) return true;
    for (u32 i = 0; i < expr->when_clauses.size(); i++) {
        if (expr_contains_subquery(expr->when_clauses[i].first.get())) return true;
        if (expr_contains_subquery(expr->when_clauses[i].second.get())) return true;
    }
    if (expr->else_expr && expr_contains_subquery(expr->else_expr.get())) return true;
    return false;
}

static bool btree_supports_type(TypeId type) {
    return type == TypeId::kBool || type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble ||
           type == TypeId::kVarchar || type == TypeId::kTimestamp ||
           type == TypeId::kDatetime || type == TypeId::kNull;
}

static bool is_datetime_type(TypeId type) {
    return type == TypeId::kTimestamp || type == TypeId::kDatetime;
}

static bool cast_literal_for_column(const Value& value, TypeId target, Value* out) {
    if (!out) return false;
    if (value.is_null() || value.type_id() == target) {
        *out = value;
        return true;
    }
    if (!is_datetime_type(target)) {
        *out = value;
        return true;
    }
    Value casted = value.cast_to(target);
    if (casted.is_null()) return false;
    *out = casted;
    return true;
}

static bool table_stats_fresh(const TableEntry* table) {
    if (!table || !table->stats_valid || table->stat_num_tuples == 0) return false;
    u64 current = table->num_tuples;
    u64 analyzed = table->stat_num_tuples;
    u64 diff = current > analyzed ? current - analyzed : analyzed - current;
    u64 base = analyzed > 0 ? analyzed : 1;
    return diff * 100 <= base * 20;
}

static bool extract_index_eq_predicate(const Expression* expr, const Schema& schema,
                                       u32* column_idx, Value* key) {
    if (!expr || expr->type != ExprType::kBinaryOp || expr->op != "=") return false;

    const Expression* col_expr = nullptr;
    const Expression* lit_expr = nullptr;
    if (expr->left && expr->left->type == ExprType::kColumnRef &&
        expr->right && expr->right->type == ExprType::kLiteral) {
        col_expr = expr->left.get();
        lit_expr = expr->right.get();
    } else if (expr->right && expr->right->type == ExprType::kColumnRef &&
               expr->left && expr->left->type == ExprType::kLiteral) {
        col_expr = expr->right.get();
        lit_expr = expr->left.get();
    } else {
        return false;
    }

    int idx = col_expr->table_name.empty()
              ? schema.get_column_index(col_expr->column_name)
              : schema.get_column_index(col_expr->table_name, col_expr->column_name);
    if (idx < 0) return false;

    *column_idx = static_cast<u32>(idx);
    if (!cast_literal_for_column(lit_expr->literal_value,
                                 schema.get_column(*column_idx).type, key)) {
        return false;
    }
    return true;
}

static bool find_index_eq_predicate(const Expression* expr, const Schema& schema,
                                    u32* column_idx, Value* key, bool* covers_whole_expr) {
    if (extract_index_eq_predicate(expr, schema, column_idx, key)) {
        *covers_whole_expr = true;
        return true;
    }
    if (expr && expr->type == ExprType::kBinaryOp && expr->op == "AND") {
        if (find_index_eq_predicate(expr->left.get(), schema, column_idx, key, covers_whole_expr)) {
            *covers_whole_expr = false;
            return true;
        }
        if (find_index_eq_predicate(expr->right.get(), schema, column_idx, key, covers_whole_expr)) {
            *covers_whole_expr = false;
            return true;
        }
    }
    return false;
}

static void collect_eq_literals(const Expression* expr, const Schema& schema,
                                HashMap<u32, Value>& eq_values) {
    if (!expr) return;
    u32 column_idx = 0;
    Value key;
    if (extract_index_eq_predicate(expr, schema, &column_idx, &key)) {
        eq_values[column_idx] = key;
        return;
    }
    if (expr->type == ExprType::kBinaryOp && expr->op == "AND") {
        collect_eq_literals(expr->left.get(), schema, eq_values);
        collect_eq_literals(expr->right.get(), schema, eq_values);
    }
}

static bool build_composite_prefix_key(const Expression* expr, const Schema& schema,
                                       const IndexEntry& index, IndexKey* search_key,
                                       bool* covers_full_index) {
    if (!expr || !search_key || index.key_columns.size() <= 1) return false;

    HashMap<u32, Value> eq_values;
    collect_eq_literals(expr, schema, eq_values);

    Vector<Value> values;
    for (u32 i = 0; i < index.key_columns.size(); i++) {
        Value* value = eq_values.find(index.key_columns[i]);
        if (!value) break;
        values.push_back(*value);
    }
    if (values.empty()) return false;
    *search_key = IndexKey::from_values(values);
    if (covers_full_index) *covers_full_index = values.size() == index.key_columns.size();
    return true;
}

static bool extract_index_bound_predicate(const Expression* expr, const Schema& schema,
                                          u32* column_idx, Value* key,
                                          bool* is_lower, bool* inclusive) {
    if (!expr || expr->type != ExprType::kBinaryOp) return false;
    if (!(expr->op == ">=" || expr->op == ">" || expr->op == "<=" || expr->op == "<")) {
        return false;
    }

    const Expression* col_expr = nullptr;
    const Expression* lit_expr = nullptr;
    bool literal_on_left = false;
    if (expr->left && expr->left->type == ExprType::kColumnRef &&
        expr->right && expr->right->type == ExprType::kLiteral) {
        col_expr = expr->left.get();
        lit_expr = expr->right.get();
    } else if (expr->right && expr->right->type == ExprType::kColumnRef &&
               expr->left && expr->left->type == ExprType::kLiteral) {
        col_expr = expr->right.get();
        lit_expr = expr->left.get();
        literal_on_left = true;
    } else {
        return false;
    }

    int idx = col_expr->table_name.empty()
              ? schema.get_column_index(col_expr->column_name)
              : schema.get_column_index(col_expr->table_name, col_expr->column_name);
    if (idx < 0) return false;

    String op = expr->op;
    if (literal_on_left) {
        if (op == ">") op = "<";
        else if (op == ">=") op = "<=";
        else if (op == "<") op = ">";
        else if (op == "<=") op = ">=";
    }

    *column_idx = static_cast<u32>(idx);
    if (!cast_literal_for_column(lit_expr->literal_value,
                                 schema.get_column(*column_idx).type, key)) {
        return false;
    }
    *is_lower = (op == ">=" || op == ">");
    *inclusive = (op == ">=" || op == "<=");
    return true;
}

static bool extract_index_range_predicate(const Expression* expr, const Schema& schema,
                                          u32* column_idx, Value* low, Value* high,
                                          bool* covers_whole_expr) {
    if (!expr || expr->type != ExprType::kBinaryOp) return false;

    // Handle top-level AND
    if (expr->op == "AND") {
        // Try direct children first
        u32 left_col = 0, right_col = 0;
        Value left_key, right_key;
        bool left_lower = false, right_lower = false;
        bool left_inc = false, right_inc = false;
        if (extract_index_bound_predicate(expr->left.get(), schema, &left_col, &left_key,
                                          &left_lower, &left_inc) &&
            extract_index_bound_predicate(expr->right.get(), schema, &right_col, &right_key,
                                          &right_lower, &right_inc) &&
            left_col == right_col && left_lower != right_lower) {
            *column_idx = left_col;
            if (left_lower) { *low = left_key; *high = right_key; }
            else { *low = right_key; *high = left_key; }
            *covers_whole_expr = left_inc && right_inc;
            return true;
        }
        // Recurse into children if they are also AND. CRITICAL: when we
        // descend into a child AND we are matching only a sub-portion of the
        // original predicate — the outer AND has at least one other conjunct
        // (the sibling we didn't recurse into) that the chosen index range
        // does NOT cover. Force `covers_whole_expr` to false so the caller
        // keeps a residual Filter (or pushed predicate) above the IndexScan.
        // The previous bug was: a query like
        //     WHERE (id >= 2 AND id <= 4) AND v < 350
        // matched the inner BETWEEN, set covers_whole_expr=true via the
        // inner call, and silently dropped the `v < 350` conjunct.
        if (expr->left && expr->left->type == ExprType::kBinaryOp && expr->left->op == "AND") {
            if (extract_index_range_predicate(expr->left.get(), schema, column_idx, low, high, covers_whole_expr)) {
                *covers_whole_expr = false;
                return true;
            }
        }
        if (expr->right && expr->right->type == ExprType::kBinaryOp && expr->right->op == "AND") {
            if (extract_index_range_predicate(expr->right.get(), schema, column_idx, low, high, covers_whole_expr)) {
                *covers_whole_expr = false;
                return true;
            }
        }
    }
    return false;
}

static Value min_key_for_type(TypeId type) {
    switch (type) {
        case TypeId::kInt32: return Value(static_cast<i32>(-2147483647 - 1));
        case TypeId::kInt64: return Value(static_cast<i64>(INT64_MIN));
        case TypeId::kTimestamp: return Value::timestamp(static_cast<i64>(INT64_MIN));
        case TypeId::kDatetime: return Value::datetime(static_cast<i64>(INT64_MIN));
        case TypeId::kFloat: return Value(-3.402823466e+38F);
        case TypeId::kDouble: return Value(-1.7976931348623157e+308);
        case TypeId::kBool: return Value(false);
        case TypeId::kVarchar: return Value(String(""));
        default: return Value();
    }
}

static Value max_key_for_type(TypeId type) {
    switch (type) {
        case TypeId::kInt32: return Value(static_cast<i32>(2147483647));
        case TypeId::kInt64: return Value(static_cast<i64>(INT64_MAX));
        case TypeId::kTimestamp: return Value::timestamp(static_cast<i64>(INT64_MAX));
        case TypeId::kDatetime: return Value::datetime(static_cast<i64>(INT64_MAX));
        case TypeId::kFloat: return Value(3.402823466e+38F);
        case TypeId::kDouble: return Value(1.7976931348623157e+308);
        case TypeId::kBool: return Value(true);
        case TypeId::kVarchar: return Value(String("\xff\xff\xff\xff", 4));
        default: return Value();
    }
}

static bool is_hashable_equi_join(const JoinPlan* plan) {
    return plan &&
           (plan->join_type == JoinType::kInner || plan->join_type == JoinType::kLeft) &&
           plan->on_condition &&
           plan->on_condition->type == ExprType::kBinaryOp &&
           plan->on_condition->op == "=";
}

static bool expr_can_eval_on(const Expression* expr, const Schema& schema) {
    if (!expr) return true;
    switch (expr->type) {
        case ExprType::kLiteral:
        case ExprType::kStar:
            return true;
        case ExprType::kColumnRef:
            return expr->table_name.empty()
                ? schema.get_column_index(expr->column_name) >= 0
                : schema.get_column_index(expr->table_name, expr->column_name) >= 0;
        case ExprType::kBinaryOp:
            return expr_can_eval_on(expr->left.get(), schema) &&
                   expr_can_eval_on(expr->right.get(), schema);
        case ExprType::kUnaryOp:
            return expr_can_eval_on(expr->child.get(), schema);
        case ExprType::kCase:
            for (u32 i = 0; i < expr->when_clauses.size(); i++) {
                if (!expr_can_eval_on(expr->when_clauses[i].first.get(), schema) ||
                    !expr_can_eval_on(expr->when_clauses[i].second.get(), schema)) return false;
            }
            return expr_can_eval_on(expr->else_expr.get(), schema);
        case ExprType::kCast:
            return expr_can_eval_on(expr->child.get(), schema);
        case ExprType::kSubquery:
            return false;
    }
    return false;
}

static void split_conjuncts(const Expression* expr, Vector<UniquePtr<Expression>>& out) {
    if (!expr) return;
    if (expr->type == ExprType::kBinaryOp && expr->op == "AND") {
        split_conjuncts(expr->left.get(), out);
        split_conjuncts(expr->right.get(), out);
        return;
    }
    out.push_back(UniquePtr<Expression>(expr->clone()));
}

static UniquePtr<Expression> combine_conjuncts(Vector<UniquePtr<Expression>>& exprs) {
    UniquePtr<Expression> result;
    for (u32 i = 0; i < exprs.size(); i++) {
        if (!exprs[i]) continue;
        if (!result) {
            result = UniquePtr<Expression>(exprs[i].release());
        } else {
            auto* and_expr = new Expression();
            and_expr->type = ExprType::kBinaryOp;
            and_expr->op = "AND";
            and_expr->left = static_cast<UniquePtr<Expression>&&>(result);
            and_expr->right = UniquePtr<Expression>(exprs[i].release());
            result = UniquePtr<Expression>(and_expr);
        }
    }
    return result;
}

static UniquePtr<PlanNode> wrap_filter(UniquePtr<PlanNode> child, UniquePtr<Expression> predicate) {
    if (!child || !predicate) return child;
    UniquePtr<FilterPlan> filter(new FilterPlan());
    filter->output_schema = child->output_schema;
    filter->child = static_cast<UniquePtr<PlanNode>&&>(child);
    filter->predicate = static_cast<UniquePtr<Expression>&&>(predicate);
    return UniquePtr<PlanNode>(filter.release());
}

static bool extract_join_key_indices(const JoinPlan* plan, u32* left_idx, u32* right_idx) {
    if (!plan || !plan->on_condition || plan->on_condition->type != ExprType::kBinaryOp ||
        plan->on_condition->op != "=" || !plan->on_condition->left || !plan->on_condition->right) {
        return false;
    }
    const Expression* lhs = plan->on_condition->left.get();
    const Expression* rhs = plan->on_condition->right.get();
    if (lhs->type != ExprType::kColumnRef || rhs->type != ExprType::kColumnRef) return false;

    int lhs_l = lhs->table_name.empty()
        ? plan->left->output_schema.get_column_index(lhs->column_name)
        : plan->left->output_schema.get_column_index(lhs->table_name, lhs->column_name);
    int lhs_r = lhs->table_name.empty()
        ? plan->right->output_schema.get_column_index(lhs->column_name)
        : plan->right->output_schema.get_column_index(lhs->table_name, lhs->column_name);
    int rhs_l = rhs->table_name.empty()
        ? plan->left->output_schema.get_column_index(rhs->column_name)
        : plan->left->output_schema.get_column_index(rhs->table_name, rhs->column_name);
    int rhs_r = rhs->table_name.empty()
        ? plan->right->output_schema.get_column_index(rhs->column_name)
        : plan->right->output_schema.get_column_index(rhs->table_name, rhs->column_name);

    if (lhs_l >= 0 && rhs_r >= 0) {
        if (left_idx) *left_idx = static_cast<u32>(lhs_l);
        if (right_idx) *right_idx = static_cast<u32>(rhs_r);
        return true;
    }
    if (rhs_l >= 0 && lhs_r >= 0) {
        if (left_idx) *left_idx = static_cast<u32>(rhs_l);
        if (right_idx) *right_idx = static_cast<u32>(lhs_r);
        return true;
    }
    return false;
}

static bool schema_contains_expr(const Schema& schema, const Expression* expr) {
    if (!expr || expr->type != ExprType::kColumnRef) return false;
    return expr->table_name.empty()
        ? schema.get_column_index(expr->column_name) >= 0
        : schema.get_column_index(expr->table_name, expr->column_name) >= 0;
}

static bool find_expr_table_column_in_subtree(const PlanNode* plan, const Expression* expr,
                                              u32* table_id, u32* column_idx) {
    if (!plan || !expr || expr->type != ExprType::kColumnRef) return false;

    switch (plan->type) {
        case PlanNodeType::kSeqScan: {
            const auto* scan = static_cast<const SeqScanPlan*>(plan);
            int idx = expr->table_name.empty()
                ? scan->output_schema.get_column_index(expr->column_name)
                : scan->output_schema.get_column_index(expr->table_name, expr->column_name);
            if (idx < 0) return false;
            if (table_id) *table_id = scan->table_id;
            if (column_idx) *column_idx = static_cast<u32>(idx);
            return true;
        }
        case PlanNodeType::kIndexScan: {
            const auto* scan = static_cast<const IndexScanPlan*>(plan);
            int idx = expr->table_name.empty()
                ? scan->output_schema.get_column_index(expr->column_name)
                : scan->output_schema.get_column_index(expr->table_name, expr->column_name);
            if (idx < 0) return false;
            if (table_id) *table_id = scan->table_id;
            if (column_idx) *column_idx = static_cast<u32>(idx);
            return true;
        }
        case PlanNodeType::kIndexOnlyScan: {
            const auto* scan = static_cast<const IndexOnlyScanPlan*>(plan);
            int idx = expr->table_name.empty()
                ? scan->output_schema.get_column_index(expr->column_name)
                : scan->output_schema.get_column_index(expr->table_name, expr->column_name);
            if (idx < 0) return false;
            if (table_id) *table_id = scan->table_id;
            if (column_idx) *column_idx = static_cast<u32>(idx);
            return true;
        }
        case PlanNodeType::kFilter:
            return find_expr_table_column_in_subtree(static_cast<const FilterPlan*>(plan)->child.get(),
                                                     expr, table_id, column_idx);
        case PlanNodeType::kProject:
            return find_expr_table_column_in_subtree(static_cast<const ProjectPlan*>(plan)->child.get(),
                                                     expr, table_id, column_idx);
        case PlanNodeType::kLimit:
            return find_expr_table_column_in_subtree(static_cast<const LimitPlan*>(plan)->child.get(),
                                                     expr, table_id, column_idx);
        case PlanNodeType::kSort:
            return find_expr_table_column_in_subtree(static_cast<const SortPlan*>(plan)->child.get(),
                                                     expr, table_id, column_idx);
        case PlanNodeType::kDistinct:
            return find_expr_table_column_in_subtree(static_cast<const DistinctPlan*>(plan)->child.get(),
                                                     expr, table_id, column_idx);
        case PlanNodeType::kAggregate:
            return find_expr_table_column_in_subtree(static_cast<const AggregatePlan*>(plan)->child.get(),
                                                     expr, table_id, column_idx);
        case PlanNodeType::kJoin: {
            const auto* join = static_cast<const JoinPlan*>(plan);
            bool on_left = join->left && schema_contains_expr(join->left->output_schema, expr);
            bool on_right = join->right && schema_contains_expr(join->right->output_schema, expr);
            if (on_left && !on_right) {
                return find_expr_table_column_in_subtree(join->left.get(), expr, table_id, column_idx);
            }
            if (on_right && !on_left) {
                return find_expr_table_column_in_subtree(join->right.get(), expr, table_id, column_idx);
            }
            return false;
        }
        case PlanNodeType::kUnion:
        case PlanNodeType::kOneRow:
        case PlanNodeType::kInsert:
        case PlanNodeType::kDelete:
        case PlanNodeType::kUpdate:
            return false;
    }
    return false;
}

static u32 ndv_for_join_expr(Catalog* catalog, const PlanNode* subtree, const Expression* expr) {
    if (!catalog || !subtree || !expr || expr->type != ExprType::kColumnRef) return 0;
    u32 table_id = 0;
    if (!find_expr_table_column_in_subtree(subtree, expr, &table_id, nullptr)) return 0;
    TableEntry* table = catalog->get_table(table_id);
    if (!table_stats_fresh(table)) return 0;

    int base_idx = expr->table_name.empty()
        ? table->schema.get_column_index(expr->column_name)
        : table->schema.get_column_index(expr->table_name, expr->column_name);
    if (base_idx < 0 || static_cast<u32>(base_idx) >= table->col_stats.size()) return 0;
    return static_cast<u32>(table->col_stats[static_cast<u32>(base_idx)].ndv);
}

static void add_required_col(Vector<u32>* cols, u32 idx) {
    if (!cols || cols->contains(idx)) return;
    cols->push_back(idx);
}

static void collect_expr_columns(const Expression* expr, const Schema& schema, Vector<u32>* cols) {
    if (!expr || !cols) return;
    if (expr->type == ExprType::kColumnRef) {
        int idx = expr->table_name.empty()
            ? schema.get_column_index(expr->column_name)
            : schema.get_column_index(expr->table_name, expr->column_name);
        if (idx >= 0) add_required_col(cols, static_cast<u32>(idx));
        return;
    }
    collect_expr_columns(expr->left.get(), schema, cols);
    collect_expr_columns(expr->right.get(), schema, cols);
    collect_expr_columns(expr->child.get(), schema, cols);
    for (u32 i = 0; i < expr->when_clauses.size(); i++) {
        collect_expr_columns(expr->when_clauses[i].first.get(), schema, cols);
        collect_expr_columns(expr->when_clauses[i].second.get(), schema, cols);
    }
    collect_expr_columns(expr->else_expr.get(), schema, cols);
}

static void apply_scan_projection(PlanNode* plan, const Vector<u32>& cols) {
    if (!plan || cols.empty()) return;
    if (plan->type == PlanNodeType::kSeqScan) {
        auto* scan = static_cast<SeqScanPlan*>(plan);
        // A pushed-down WHERE predicate is evaluated against the columns this
        // scan emits. If the late-materialization projection drops a column the
        // predicate references, the predicate can no longer resolve it and
        // silently filters every row out (e.g. COUNT(*) over a join with a
        // single-table WHERE returns 0). Keep the predicate's columns.
        Vector<u32> needed = cols;
        collect_expr_columns(scan->pushed_predicate.get(), scan->output_schema, &needed);
        Schema projected;
        for (u32 i = 0; i < needed.size(); i++) {
            if (needed[i] < scan->output_schema.column_count()) {
                projected.add_column(scan->output_schema.get_column(needed[i]));
            }
        }
        if (projected.column_count() > 0) {
            scan->projected_columns = needed;
            scan->output_schema = projected;
            scan->optimizer_note = "late materialized join projection";
        }
    } else if (plan->type == PlanNodeType::kFilter) {
        auto* filter = static_cast<FilterPlan*>(plan);
        Vector<u32> needed = cols;
        collect_expr_columns(filter->predicate.get(), filter->child->output_schema, &needed);
        apply_scan_projection(filter->child.get(), needed);
        filter->output_schema = filter->child->output_schema;
    }
}

Optimizer::Optimizer(Catalog* catalog) : catalog_(catalog), config_() {}

Optimizer::Optimizer(Catalog* catalog, const OptimizerConfig& config)
    : catalog_(catalog), config_(config) {}

UniquePtr<PlanNode> Optimizer::optimize(UniquePtr<PlanNode> plan) {
    return optimize_node(static_cast<UniquePtr<PlanNode>&&>(plan));
}

UniquePtr<PlanNode> Optimizer::optimize_node(UniquePtr<PlanNode> plan) {
    if (!plan) return UniquePtr<PlanNode>();

    switch (plan->type) {
        case PlanNodeType::kFilter:
            return optimize_filter(UniquePtr<FilterPlan>(static_cast<FilterPlan*>(plan.release())));

        case PlanNodeType::kProject: {
            auto* p = static_cast<ProjectPlan*>(plan.get());
            p->child = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->child));
            if (p->child && p->child->type == PlanNodeType::kSeqScan) {
                auto* scan = static_cast<SeqScanPlan*>(p->child.get());
                // If the underlying scan already absorbed a Filter via
                // pushed_predicate, projection pushdown that drops columns
                // referenced by the predicate would break correctness — the
                // scan would try to evaluate the predicate against a tuple
                // whose Value slots don't include those columns. Resolve by
                // un-pushing the predicate: wrap the scan back into a Filter
                // and drop straight to the existing pipeline (Project → Filter
                // → SeqScan). The Filter then runs above the projected tuple,
                // which keeps the predicate's columns in scope. We lose the
                // pushdown for this one query shape, but no correctness gap.
                if (scan->pushed_predicate) {
                    auto unwrap = make_unique<FilterPlan>();
                    unwrap->predicate = UniquePtr<Expression>(scan->pushed_predicate.release());
                    unwrap->output_schema = scan->output_schema;
                    estimate_unary(unwrap.get(), scan, 0.5, 0.02);
                    unwrap->child = UniquePtr<PlanNode>(p->child.release());
                    p->child = UniquePtr<PlanNode>(unwrap.release());
                    break;   // re-enter normal Project handling on next visit
                }
                Vector<u32> projected;
                bool simple_columns = p->output_schema.column_count() > 0;
                if (p->expressions.empty()) {
                    for (u32 i = 0; i < p->column_indices.size(); i++) {
                        projected.push_back(p->column_indices[i]);
                    }
                } else if (p->column_indices.empty()) {
                    for (u32 i = 0; i < p->expressions.size(); i++) {
                        if (!p->expressions[i] || p->expressions[i]->type != ExprType::kColumnRef) {
                            simple_columns = false;
                            break;
                        }
                        int idx = p->expressions[i]->table_name.empty()
                            ? scan->output_schema.get_column_index(p->expressions[i]->column_name)
                            : scan->output_schema.get_column_index(p->expressions[i]->table_name,
                                                                   p->expressions[i]->column_name);
                        if (idx < 0) {
                            simple_columns = false;
                            break;
                        }
                        projected.push_back(static_cast<u32>(idx));
                    }
                } else {
                    simple_columns = false;
                }
                if (simple_columns && projected.size() == p->output_schema.column_count()) {
                    scan->projected_columns = static_cast<Vector<u32>&&>(projected);
                    scan->output_schema = p->output_schema;
                    estimate_scan(scan);
                    scan->optimizer_note = "late materialized sequential scan path";
                    return UniquePtr<PlanNode>(p->child.release());
                }
            }
            if (config_.enable_indexonlyscan && p->child &&
                p->child->type == PlanNodeType::kIndexScan) {
                auto* scan = static_cast<IndexScanPlan*>(p->child.get());
                IndexEntry* index = catalog_ ? catalog_->get_index(scan->index_id) : nullptr;
                bool projects_index_key = false;
                if (index && index->state == IndexState::kValid &&     // B5/D4
                    index->key_columns.size() == 1) {
                    if (p->column_indices.size() == 1 && p->expressions.empty()) {
                        projects_index_key = index->key_columns[0] == p->column_indices[0];
                    } else if (p->column_indices.empty() && p->expressions.size() == 1 &&
                               p->expressions[0] &&
                               p->expressions[0]->type == ExprType::kColumnRef) {
                        TableEntry* table = catalog_ ? catalog_->get_table(scan->table_id) : nullptr;
                        if (table) {
                            int col_idx = p->expressions[0]->table_name.empty()
                                ? table->schema.get_column_index(p->expressions[0]->column_name)
                                : table->schema.get_column_index(p->expressions[0]->table_name,
                                                                 p->expressions[0]->column_name);
                            projects_index_key = col_idx >= 0 &&
                                                 static_cast<u32>(col_idx) == index->key_columns[0];
                        }
                    }
                }
                if (projects_index_key) {
                    UniquePtr<IndexOnlyScanPlan> only(new IndexOnlyScanPlan());
                    only->table_id = scan->table_id;
                    only->index_id = scan->index_id;
                    only->table_name = scan->table_name;
                    only->search_key = scan->search_key;
                    only->is_range = scan->is_range;
                    only->range_high = scan->range_high;
                    only->output_schema = p->output_schema;
                    only->plan_rows = scan->plan_rows;
                    only->startup_cost = scan->startup_cost;
                    only->total_cost = scan->total_cost * 0.65;
                    only->optimizer_note = "index-only scan path";
                    return UniquePtr<PlanNode>(only.release());
                }
            }
            estimate_unary(p, p->child.get(), 1.0, 0.01);
            p->optimizer_note = "targetlist projection";
            return plan;
        }

        case PlanNodeType::kSort: {
            auto* p = static_cast<SortPlan*>(plan.get());
            p->child = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->child));
            if (p->keys.size() == 1 && p->keys[0].ascending &&
                p->keys[0].expression &&
                p->keys[0].expression->type == ExprType::kColumnRef &&
                p->child &&
                (p->child->type == PlanNodeType::kIndexScan ||
                 p->child->type == PlanNodeType::kIndexOnlyScan)) {
                u32 index_id = p->child->type == PlanNodeType::kIndexScan
                    ? static_cast<IndexScanPlan*>(p->child.get())->index_id
                    : static_cast<IndexOnlyScanPlan*>(p->child.get())->index_id;
                IndexEntry* index = catalog_ ? catalog_->get_index(index_id) : nullptr;
                if (index && index->state == IndexState::kValid &&      // B5/D4
                    index->key_columns.size() == 1) {
                    int sort_idx = p->keys[0].expression->table_name.empty()
                        ? p->child->output_schema.get_column_index(p->keys[0].expression->column_name)
                        : p->child->output_schema.get_column_index(p->keys[0].expression->table_name,
                                                                   p->keys[0].expression->column_name);
                    if (sort_idx >= 0 && static_cast<u32>(sort_idx) == index->key_columns[0]) {
                        p->child->optimizer_note = p->child->optimizer_note + " + order-preserving";
                        return UniquePtr<PlanNode>(p->child.release());
                    }
                }
            }
            p->plan_rows = p->child ? p->child->plan_rows : 0.0;
            p->startup_cost = p->child ? p->child->total_cost : 0.0;
            p->total_cost = p->startup_cost + sort_cost(p->plan_rows);
            p->optimizer_note = "explicit sort path";
            return plan;
        }

        case PlanNodeType::kLimit: {
            auto* p = static_cast<LimitPlan*>(plan.get());
            p->child = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->child));
            double child_rows = p->child ? p->child->plan_rows : 0.0;
            p->plan_rows = child_rows;
            if (p->limit >= 0 && p->plan_rows > p->limit) p->plan_rows = p->limit;
            p->startup_cost = p->child ? p->child->startup_cost : 0.0;
            p->total_cost = (p->child ? p->child->total_cost : 0.0) * (child_rows > 0.0 ? p->plan_rows / child_rows : 1.0);
            p->optimizer_note = "limit row goal";
            return plan;
        }

        case PlanNodeType::kDistinct: {
            auto* p = static_cast<DistinctPlan*>(plan.get());
            p->child = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->child));
            estimate_unary(p, p->child.get(), 0.5, 0.04);
            p->optimizer_note = "hash distinct";
            return plan;
        }

        case PlanNodeType::kAggregate: {
            auto* p = static_cast<AggregatePlan*>(plan.get());
            p->child = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->child));
            bool count_only = p->group_by.empty() && !p->having && !p->aggregates.empty();
            for (u32 a = 0; a < p->aggregates.size(); a++) {
                if (p->aggregates[a].func != AggFunc::kCount ||
                    p->aggregates[a].distinct ||
                    p->aggregates[a].argument) {
                    count_only = false;
                    break;
                }
            }
            if (count_only && p->child && p->child->type == PlanNodeType::kJoin) {
                auto* join = static_cast<JoinPlan*>(p->child.get());
                u32 left_key = 0, right_key = 0;
                if (extract_join_key_indices(join, &left_key, &right_key)) {
                    Vector<u32> left_cols;
                    Vector<u32> right_cols;
                    add_required_col(&left_cols, left_key);
                    add_required_col(&right_cols, right_key);
                    apply_scan_projection(join->left.get(), left_cols);
                    apply_scan_projection(join->right.get(), right_cols);
                    Schema joined;
                    for (u32 i = 0; i < join->left->output_schema.column_count(); i++) {
                        joined.add_column(join->left->output_schema.get_column(i));
                    }
                    for (u32 i = 0; i < join->right->output_schema.column_count(); i++) {
                        joined.add_column(join->right->output_schema.get_column(i));
                    }
                    join->output_schema = joined;
                    join->optimizer_note = join->optimizer_note + " + count-only projection";
                }
            }
            double child_rows = p->child ? p->child->plan_rows : 0.0;

            // ================================================================
            // Aggregate row estimation using NDV statistics
            // No GROUP BY: 1 row
            // GROUP BY on column: rows = min(NDV, child_rows)
            // ================================================================
            double agg_selectivity = 0.1; // default
            if (p->group_by.empty()) {
                agg_selectivity = 1.0 / child_rows; // 1 row output
                if (child_rows == 0) agg_selectivity = 1.0;
            } else if (catalog_) {
                // Try to get NDV from the first GROUP BY column
                u32 max_ndv = 0;
                for (u32 i = 0; i < p->group_by.size(); i++) {
                    if (p->group_by[i] && p->group_by[i]->type == ExprType::kColumnRef) {
                        // Find the table from the child plan
                        u32 tid = 0;
                    if (p->child) {
                        const PlanNode* child = p->child.get();
                        if (child->type == PlanNodeType::kSeqScan) {
                            tid = static_cast<const SeqScanPlan*>(child)->table_id;
                        } else if (child->type == PlanNodeType::kIndexScan) {
                            tid = static_cast<const IndexScanPlan*>(child)->table_id;
                        } else if (child->type == PlanNodeType::kIndexOnlyScan) {
                            tid = static_cast<const IndexOnlyScanPlan*>(child)->table_id;
                        } else if (child->type == PlanNodeType::kFilter) {
                            const PlanNode* gc = static_cast<const FilterPlan*>(child)->child.get();
                            if (gc && gc->type == PlanNodeType::kSeqScan)
                                tid = static_cast<const SeqScanPlan*>(gc)->table_id;
                            else if (gc && gc->type == PlanNodeType::kIndexScan)
                                tid = static_cast<const IndexScanPlan*>(gc)->table_id;
                            else if (gc && gc->type == PlanNodeType::kIndexOnlyScan)
                                tid = static_cast<const IndexOnlyScanPlan*>(gc)->table_id;
                        } else if (child->type == PlanNodeType::kProject) {
                            const PlanNode* gc = static_cast<const ProjectPlan*>(child)->child.get();
                            if (gc && gc->type == PlanNodeType::kSeqScan)
                                tid = static_cast<const SeqScanPlan*>(gc)->table_id;
                            else if (gc && gc->type == PlanNodeType::kIndexScan)
                                tid = static_cast<const IndexScanPlan*>(gc)->table_id;
                            else if (gc && gc->type == PlanNodeType::kIndexOnlyScan)
                                tid = static_cast<const IndexOnlyScanPlan*>(gc)->table_id;
                        }
                    }
                        TableEntry* table = catalog_->get_table(tid);
                        if (table_stats_fresh(table)) {
                            int idx = table->schema.get_column_index(p->group_by[i]->column_name);
                            if (idx >= 0 && static_cast<u32>(idx) < table->col_stats.size()) {
                                u32 ndv = table->col_stats[idx].ndv;
                                if (ndv > max_ndv) max_ndv = ndv;
                            }
                        }
                    }
                }
                if (max_ndv > 0) {
                    // GROUP BY output rows = min(NDV, input_rows)
                    double estimated_groups = static_cast<double>(max_ndv);
                    if (estimated_groups > child_rows) estimated_groups = child_rows;
                    agg_selectivity = estimated_groups / child_rows;
                    if (agg_selectivity < 0.001) agg_selectivity = 0.001;
                    if (agg_selectivity > 1.0) agg_selectivity = 1.0;
                }
            }
            estimate_unary(p, p->child.get(), agg_selectivity, 0.05);
            if (p->plan_rows < 1.0) p->plan_rows = 1.0;
            p->optimizer_note = p->group_by.empty() ? "plain aggregate" : "hash aggregate";
            return plan;
        }

        case PlanNodeType::kUnion: {
            auto* p = static_cast<UnionPlan*>(plan.get());
            p->left = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->left));
            p->right = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->right));
            estimate_binary(p, p->left.get(), p->right.get(), p->all ? 1.0 : 0.5);
            p->optimizer_note = p->all ? "append" : "append + unique";
            return plan;
        }

        case PlanNodeType::kJoin: {
            auto* p = static_cast<JoinPlan*>(plan.get());
            p->left = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->left));
            p->right = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->right));

            double left_rows = p->left ? p->left->plan_rows : 1000.0;
            double right_rows = p->right ? p->right->plan_rows : 1000.0;
            double left_cost = p->left ? p->left->total_cost : 0.0;
            double right_cost = p->right ? p->right->total_cost : 0.0;

            // ================================================================
            // Join selectivity estimation (based on standard DB optimizer theory)
            // For equality joins: selectivity ≈ 1/max(NDV_left, NDV_right)
            // Default fallback: 0.1 (more conservative than product)
            // ================================================================
            double join_selectivity = p->on_condition ? 0.1 : 1.0;
            if (p->on_condition && p->on_condition->type == ExprType::kBinaryOp &&
                p->on_condition->op == "=" &&
                p->on_condition->left && p->on_condition->right) {
                // Try to get NDV from the correct base table on each side.
                u32 left_ndv = ndv_for_join_expr(catalog_, p->left.get(), p->on_condition->left.get());
                u32 right_ndv = ndv_for_join_expr(catalog_, p->right.get(), p->on_condition->right.get());
                if (left_ndv == 0 || right_ndv == 0) {
                    left_ndv = ndv_for_join_expr(catalog_, p->left.get(), p->on_condition->right.get());
                    right_ndv = ndv_for_join_expr(catalog_, p->right.get(), p->on_condition->left.get());
                }

                if (left_ndv > 0 && right_ndv > 0) {
                    double effective_left_ndv = static_cast<double>(left_ndv);
                    double effective_right_ndv = static_cast<double>(right_ndv);
                    if (left_rows > 0.0 && effective_left_ndv > left_rows) effective_left_ndv = left_rows;
                    if (right_rows > 0.0 && effective_right_ndv > right_rows) effective_right_ndv = right_rows;
                    double max_ndv = effective_left_ndv > effective_right_ndv
                        ? effective_left_ndv : effective_right_ndv;
                    if (max_ndv < 1.0) max_ndv = 1.0;
                    join_selectivity = 1.0 / max_ndv;
                }
            }
            double output_rows = static_cast<double>(left_rows) * static_cast<double>(right_rows) * join_selectivity;
            if (output_rows < 1.0) output_rows = 1.0;
            if (p->join_type == JoinType::kLeft && output_rows < left_rows) output_rows = left_rows;
            if (output_rows > static_cast<double>(left_rows) * static_cast<double>(right_rows))
                output_rows = static_cast<double>(left_rows) * static_cast<double>(right_rows);

            // ================================================================
            // Join cost estimation (standard cost model)
            // Hash Join: build + probe + output
            //   Cost = right_cost (build) + left_cost (probe) +
            //          right_rows * hash_rate + left_rows * probe_rate
            // Nested Loop: for each left row, scan right
            //   Cost = left_cost + left_rows * (right_cost + right_rows * probe_rate)
            // ================================================================
            double hash_build_rate = 0.04;    // cost to hash right table
            double hash_probe_rate = 0.02;    // cost to probe each left row
            double nl_probe_rate = 0.02;      // cost to compare each left-right pair

            double hash_cost = left_cost + right_cost + right_rows * hash_build_rate + left_rows * hash_probe_rate;
            double nested_cost = left_cost + left_rows * right_cost + left_rows * right_rows * nl_probe_rate;
            double index_lookup_cost = 1e100;
            u32 lookup_index_id = 0;
            bool can_index_lookup = false;
            u32 left_key_idx = 0, right_key_idx = 0;
            if (p->join_type == JoinType::kInner || p->join_type == JoinType::kLeft) {
                if (extract_join_key_indices(p, &left_key_idx, &right_key_idx) &&
                    p->right && p->right->type == PlanNodeType::kSeqScan) {
                    auto* right_scan = static_cast<SeqScanPlan*>(p->right.get());
                    if (has_usable_index(right_scan->table_id, right_key_idx, &lookup_index_id)) {
                        double expected_matches = right_rows > 0.0 ? output_rows / (left_rows > 0.0 ? left_rows : 1.0) : 1.0;
                        if (expected_matches < 1.0) expected_matches = 1.0;
                        double lookup_startup = config_.remote_storage
                            ? config_.remote_round_trip_cost + config_.remote_random_page_cost
                            : 0.15;
                        double lookup_tuple_cost = config_.remote_storage ? 0.08 : 0.02;
                        index_lookup_cost = left_cost + left_rows * (lookup_startup + expected_matches * lookup_tuple_cost);
                        can_index_lookup = true;
                    }
                }
            }

            // ================================================================
            // Join hint: cost adjustment (not override!)
            // hint_weight > 0: favors hash join (reduce hash cost)
            // hint_weight < 0: favors nested loop (reduce nested cost)
            // The optimizer still picks the cheaper option.
            // ================================================================
            if (p->hint_weight > 0.0) {
                hash_cost *= (1.0 - p->hint_weight * 0.1);  // e.g., hint=1 → 10% discount
            } else if (p->hint_weight < 0.0) {
                nested_cost *= (1.0 + p->hint_weight * 0.1); // e.g., hint=-1 → 10% discount
            }

            // For LEFT/RIGHT JOIN: hash join is more efficient
            if (p->join_type == JoinType::kLeft && right_rows > 1000) {
                hash_cost *= 0.95;  // Small bonus for hash join on large outer joins
            }

            if (can_index_lookup &&
                (index_lookup_cost <= hash_cost || left_rows <= 1024.0 || left_rows * 10.0 <= right_rows) &&
                index_lookup_cost <= nested_cost) {
                p->algorithm = JoinAlgorithm::kIndexLookup;
                p->lookup_inner_table_id = static_cast<SeqScanPlan*>(p->right.get())->table_id;
                p->lookup_inner_index_id = lookup_index_id;
                p->hash_build_left = false;
                p->startup_cost = left_cost + 0.15;
                p->total_cost = index_lookup_cost;
                p->optimizer_note = "index lookup join";
            } else if (config_.enable_hashjoin && is_hashable_equi_join(p) && hash_cost <= nested_cost) {
                p->algorithm = JoinAlgorithm::kHash;
                p->hash_build_left = p->join_type == JoinType::kInner && left_rows <= right_rows;
                double build_cost = p->hash_build_left ? left_cost : right_cost;
                double build_rows = p->hash_build_left ? left_rows : right_rows;
                p->startup_cost = build_cost + build_rows * hash_build_rate;
                p->total_cost = hash_cost;
                p->optimizer_note = p->hash_build_left ? "hash join build=left" : "hash join build=right";
            } else {
                p->algorithm = JoinAlgorithm::kNestedLoop;
                p->startup_cost = left_cost + right_cost;
                p->total_cost = nested_cost;
                p->optimizer_note = "nested loop";
            }
            p->plan_rows = output_rows;
            return plan;
        }

        case PlanNodeType::kDelete: {
            auto* p = static_cast<DeletePlan*>(plan.get());
            p->child = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->child));
            estimate_unary(p, p->child.get(), 1.0, 0.02);
            p->optimizer_note = "delete";
            return plan;
        }

        case PlanNodeType::kUpdate: {
            auto* p = static_cast<UpdatePlan*>(plan.get());
            // UPDATE materialises target RIDs before mutating the heap, so
            // index scans over the column being updated are Halloween-safe.
            // Let the normal scan optimizer choose equality/range paths for
            // both HOT and non-HOT updates.
            p->child = optimize_node(static_cast<UniquePtr<PlanNode>&&>(p->child));
            estimate_unary(p, p->child.get(), 1.0, 0.03);
            p->optimizer_note = "update";
            return plan;
        }

        case PlanNodeType::kInsert:
            plan->plan_rows = static_cast<InsertPlan*>(plan.get())->values.size();
            plan->startup_cost = 0.0;
            plan->total_cost = plan->plan_rows * 0.02;
            plan->optimizer_note = "values insert";
            return plan;

        case PlanNodeType::kSeqScan:
        case PlanNodeType::kIndexScan:
        case PlanNodeType::kIndexOnlyScan:
        case PlanNodeType::kOneRow:
            estimate_scan(plan.get());
            return plan;
    }

    return plan;
}

UniquePtr<PlanNode> Optimizer::optimize_filter(UniquePtr<FilterPlan> filter) {
    filter->child = optimize_node(static_cast<UniquePtr<PlanNode>&&>(filter->child));
    if (filter->child && filter->child->type == PlanNodeType::kJoin) {
        UniquePtr<PlanNode> join_node(filter->child.release());
        auto* join = static_cast<JoinPlan*>(join_node.get());
        Vector<UniquePtr<Expression>> conjuncts;
        Vector<UniquePtr<Expression>> left_preds;
        Vector<UniquePtr<Expression>> right_preds;
        Vector<UniquePtr<Expression>> residual_preds;
        split_conjuncts(filter->predicate.get(), conjuncts);
        for (u32 i = 0; i < conjuncts.size(); i++) {
            bool on_left = expr_can_eval_on(conjuncts[i].get(), join->left->output_schema);
            bool on_right = expr_can_eval_on(conjuncts[i].get(), join->right->output_schema);
            if (on_left && !on_right) {
                left_preds.push_back(UniquePtr<Expression>(conjuncts[i].release()));
            } else if (join->join_type == JoinType::kInner && on_right && !on_left) {
                right_preds.push_back(UniquePtr<Expression>(conjuncts[i].release()));
            } else {
                residual_preds.push_back(UniquePtr<Expression>(conjuncts[i].release()));
            }
        }

        UniquePtr<Expression> left_pred = combine_conjuncts(left_preds);
        UniquePtr<Expression> right_pred = combine_conjuncts(right_preds);
        if (left_pred) {
            join->left = optimize_node(wrap_filter(static_cast<UniquePtr<PlanNode>&&>(join->left),
                                                   static_cast<UniquePtr<Expression>&&>(left_pred)));
        }
        if (right_pred) {
            join->right = optimize_node(wrap_filter(static_cast<UniquePtr<PlanNode>&&>(join->right),
                                                    static_cast<UniquePtr<Expression>&&>(right_pred)));
        }
        join_node = optimize_node(static_cast<UniquePtr<PlanNode>&&>(join_node));

        UniquePtr<Expression> residual = combine_conjuncts(residual_preds);
        if (!residual) return join_node;
        filter->child = static_cast<UniquePtr<PlanNode>&&>(join_node);
        filter->predicate = static_cast<UniquePtr<Expression>&&>(residual);
        filter->output_schema = filter->child->output_schema;
    }
    if (config_.enable_indexscan && filter->child && filter->child->type == PlanNodeType::kSeqScan) {
        bool predicate_satisfied = false;
        auto index_path = choose_index_path(static_cast<const SeqScanPlan*>(filter->child.get()),
                                            filter->predicate.get(), true,
                                            &predicate_satisfied);
        if (index_path) {
            index_path = optimize_node(static_cast<UniquePtr<PlanNode>&&>(index_path));
            if (predicate_satisfied) {
                index_path->output_schema = filter->output_schema;
                return index_path;
            }
            filter->child = UniquePtr<PlanNode>(index_path.release());
        }
    }

    // W18: Use statistics for filter selectivity estimation
    u32 filter_table_id = 0;
    if (filter->child && filter->child->type == PlanNodeType::kSeqScan) {
        filter_table_id = static_cast<SeqScanPlan*>(filter->child.get())->table_id;
    } else if (filter->child && filter->child->type == PlanNodeType::kIndexScan) {
        filter_table_id = static_cast<IndexScanPlan*>(filter->child.get())->table_id;
    } else if (filter->child && filter->child->type == PlanNodeType::kIndexOnlyScan) {
        filter_table_id = static_cast<IndexOnlyScanPlan*>(filter->child.get())->table_id;
    }
    double selectivity = estimate_selectivity(filter->predicate.get(), filter_table_id);
    estimate_unary(filter.get(), filter->child.get(), selectivity, 0.02);
    filter->optimizer_note = "predicate filter";

    // Filter pushdown: when the residual filter sits directly on top of a
    // SeqScan or IndexScan, fold its predicate into the scan's
    // `pushed_predicate` and drop the Filter node entirely. The scan then
    // evaluates the predicate inline on the visible tuple, eliminating the
    // per-row ExecResult move + virtual-call boundary that a separate
    // Filter operator imposes. Index range bounds chosen above are kept;
    // the residual predicate covers conjuncts the range couldn't capture.
    //
    // Caveats:
    //   - Predicates containing subqueries (IN/NOT IN/scalar) are NOT pushed
    //     down — the SubqueryInExecutor path in the ExecutorFactory needs to
    //     see them as a FilterPlan to wire the inner plan correctly. The
    //     scalar-subquery materialisation runs at executor-build time and
    //     also expects a FilterPlan parent.
    if (filter->child && filter->child->type == PlanNodeType::kSeqScan &&
        !expr_contains_subquery(filter->predicate.get())) {
        auto* scan = static_cast<SeqScanPlan*>(filter->child.get());
        if (!scan->pushed_predicate) {
            scan->pushed_predicate = UniquePtr<Expression>(filter->predicate->clone());
        } else {
            // Combine existing pushdown with the new conjunct under AND.
            auto combined = make_unique<Expression>();
            combined->type = ExprType::kBinaryOp;
            combined->op = "AND";
            combined->left = UniquePtr<Expression>(scan->pushed_predicate.release());
            combined->right = UniquePtr<Expression>(filter->predicate->clone());
            scan->pushed_predicate = UniquePtr<Expression>(combined.release());
        }
        scan->output_schema = filter->output_schema;
        scan->plan_rows = filter->plan_rows;
        scan->startup_cost = filter->startup_cost;
        scan->total_cost = filter->total_cost;
        scan->optimizer_note = "sequential scan with pushed predicate";
        return UniquePtr<PlanNode>(filter->child.release());
    }
    if (filter->child && filter->child->type == PlanNodeType::kIndexScan &&
        !expr_contains_subquery(filter->predicate.get())) {
        auto* scan = static_cast<IndexScanPlan*>(filter->child.get());
        if (!scan->pushed_predicate) {
            scan->pushed_predicate = UniquePtr<Expression>(filter->predicate->clone());
        } else {
            auto combined = make_unique<Expression>();
            combined->type = ExprType::kBinaryOp;
            combined->op = "AND";
            combined->left = UniquePtr<Expression>(scan->pushed_predicate.release());
            combined->right = UniquePtr<Expression>(filter->predicate->clone());
            scan->pushed_predicate = UniquePtr<Expression>(combined.release());
        }
        scan->output_schema = filter->output_schema;
        scan->plan_rows = filter->plan_rows;
        scan->startup_cost = filter->startup_cost;
        scan->total_cost = filter->total_cost;
        scan->optimizer_note = "btree scan with pushed residual predicate";
        return UniquePtr<PlanNode>(filter->child.release());
    }

    return UniquePtr<PlanNode>(filter.release());
}

void Optimizer::estimate_scan(PlanNode* plan) {
    if (!plan) return;
    if (plan->type == PlanNodeType::kOneRow) {
        plan->plan_rows = 1.0;
        plan->startup_cost = 0.0;
        plan->total_cost = 0.01;
        plan->optimizer_note = "constant one-row path";
        return;
    }
    if (plan->type == PlanNodeType::kSeqScan) {
        auto* p = static_cast<SeqScanPlan*>(plan);
        plan->plan_rows = estimate_table_rows(p->table_id);
        double pages = 1.0;
        TableEntry* table = catalog_ ? catalog_->get_table(p->table_id) : nullptr;
        if (table_stats_fresh(table) && table->stat_num_pages > 0) pages = static_cast<double>(table->stat_num_pages);
        else if (table && table->num_pages > 0) pages = static_cast<double>(table->num_pages);
        double page_cost = config_.remote_storage ? config_.remote_seq_page_cost : config_.local_seq_page_cost;
        plan->startup_cost = 0.0;
        plan->total_cost = pages * page_cost + plan->plan_rows * 0.05;
        plan->optimizer_note = config_.remote_storage
            ? "sequential scan path remote-cost"
            : "sequential scan path";
        return;
    }
    if (plan->type == PlanNodeType::kIndexScan || plan->type == PlanNodeType::kIndexOnlyScan) {
        u32 table_id = plan->type == PlanNodeType::kIndexScan
            ? static_cast<IndexScanPlan*>(plan)->table_id
            : static_cast<IndexOnlyScanPlan*>(plan)->table_id;
        u32 index_id = plan->type == PlanNodeType::kIndexScan
            ? static_cast<IndexScanPlan*>(plan)->index_id
            : static_cast<IndexOnlyScanPlan*>(plan)->index_id;
        bool is_range = plan->type == PlanNodeType::kIndexScan
            ? static_cast<IndexScanPlan*>(plan)->is_range
            : static_cast<IndexOnlyScanPlan*>(plan)->is_range;
        double table_rows = estimate_table_rows(table_id);
        IndexEntry* index = catalog_ ? catalog_->get_index(index_id) : nullptr;
        bool unique_lookup = index && index->is_unique && !is_range;

        // ================================================================
        // Index scan row estimation using NDV statistics
        // Equality: selectivity = 1/NDV → rows = total * 1/NDV
        // Range:    selectivity = min(0.33, range_fraction) → rows = total * selectivity
        // ================================================================
        double idx_selectivity = 0.05; // default fallback
        if (unique_lookup) {
            idx_selectivity = 1.0 / table_rows;  // unique lookup: 1 row
        } else if (catalog_ && index) {
            u32 col_idx = index->key_columns[0];
            TableEntry* table = catalog_->get_table(table_id);
            if (table_stats_fresh(table) && col_idx < table->col_stats.size()) {
                const ColumnStats& stats = table->col_stats[col_idx];
                    if (!is_range) {
                    // Equality: 1/NDV
                    if (stats.ndv > 0) {
                        idx_selectivity = 1.0 / static_cast<double>(stats.ndv);
                    }
                } else {
                    // Range: 1/3 default
                    idx_selectivity = 0.33;
                }
            } else {
                idx_selectivity = is_range ? 0.33 : 0.05;
            }
        } else {
            idx_selectivity = is_range ? 0.33 : 0.05;
        }

        plan->plan_rows = table_rows * idx_selectivity;
        if (plan->plan_rows < 1.0) plan->plan_rows = 1.0;
        double random_cost = config_.remote_storage
            ? config_.remote_random_page_cost + config_.remote_round_trip_cost
            : config_.local_random_page_cost;
        plan->startup_cost = config_.remote_storage ? config_.remote_round_trip_cost : 0.15;
        plan->total_cost = plan->startup_cost + plan->plan_rows * (0.02 + random_cost);
        plan->optimizer_note = is_range
            ? (config_.remote_storage ? "btree range path remote-cost" : "btree range path")
            : (config_.remote_storage ? "btree equality path remote-cost" : "btree equality path");
    }
}

void Optimizer::estimate_unary(PlanNode* plan, const PlanNode* child, double selectivity, double cpu) {
    double child_rows = child ? child->plan_rows : 0.0;
    double child_cost = child ? child->total_cost : 0.0;
    plan->plan_rows = child_rows * selectivity;
    if (child_rows > 0.0 && plan->plan_rows < 1.0) plan->plan_rows = 1.0;
    plan->startup_cost = child ? child->startup_cost : 0.0;
    plan->total_cost = child_cost + child_rows * cpu;
}

void Optimizer::estimate_binary(PlanNode* plan, const PlanNode* left, const PlanNode* right, double selectivity) {
    double left_rows = left ? left->plan_rows : 0.0;
    double right_rows = right ? right->plan_rows : 0.0;
    plan->plan_rows = (left_rows + right_rows) * selectivity;
    if ((left_rows + right_rows) > 0.0 && plan->plan_rows < 1.0) plan->plan_rows = 1.0;
    plan->startup_cost = (left ? left->startup_cost : 0.0) + (right ? right->startup_cost : 0.0);
    plan->total_cost = (left ? left->total_cost : 0.0) + (right ? right->total_cost : 0.0) +
                       (left_rows + right_rows) * 0.03;
}

double Optimizer::estimate_table_rows(u32 table_id) const {
    TableEntry* table = catalog_ ? catalog_->get_table(table_id) : nullptr;
    if (table_stats_fresh(table)) return static_cast<double>(table->stat_num_tuples);
    if (table && table->num_tuples > 0) return static_cast<double>(table->num_tuples);
    return 1000.0;
}

double Optimizer::estimate_selectivity(const Expression* predicate, u32 table_id) const {
    // Default fallback
    if (!predicate) return 1.0;

    // AND/OR: combine selectivities
    if (predicate->type == ExprType::kBinaryOp && predicate->op == "AND") {
        double left = estimate_selectivity(predicate->left.get(), table_id);
        double right = estimate_selectivity(predicate->right.get(), table_id);
        return left * right;
    }
    if (predicate->type == ExprType::kBinaryOp && predicate->op == "OR") {
        double left = estimate_selectivity(predicate->left.get(), table_id);
        double right = estimate_selectivity(predicate->right.get(), table_id);
        return left + right - (left * right);
    }

    // IS NULL / IS NOT NULL. The AST stores these unary ops as "IS_NULL" /
    // "IS_NOT_NULL" (see parser); matching "IS NULL" with a space meant these
    // branches never fired, so null predicates fell through to the generic
    // default selectivity and could mis-cost plans.
    if (predicate->type == ExprType::kUnaryOp &&
        (predicate->op == "IS_NULL" || predicate->op == "IS_NOT_NULL")) {
        double null_sel = 0.1;  // default NULL fraction when stats are absent
        if (predicate->child && predicate->child->type == ExprType::kColumnRef) {
            TableEntry* table = catalog_ ? catalog_->get_table(table_id) : nullptr;
            if (table_stats_fresh(table) && table->num_tuples > 0) {
                int col_idx = table->schema.get_column_index(predicate->child->column_name);
                if (col_idx >= 0 && static_cast<u32>(col_idx) < table->col_stats.size()) {
                    const ColumnStats& stats = table->col_stats[col_idx];
                    null_sel = static_cast<double>(stats.null_count) /
                               static_cast<double>(table->num_tuples);
                }
            }
        }
        return predicate->op == "IS_NULL" ? null_sel : (1.0 - null_sel);
    }

    // Binary comparison with column reference
    if (predicate->type == ExprType::kBinaryOp && predicate->left && predicate->right) {
        bool col_on_left = (predicate->left->type == ExprType::kColumnRef);
        bool col_on_right = (predicate->right->type == ExprType::kColumnRef);

        if (col_on_left || col_on_right) {
            const Expression* col_expr = col_on_left ? predicate->left.get() : predicate->right.get();

            TableEntry* table = catalog_ ? catalog_->get_table(table_id) : nullptr;
        if (table_stats_fresh(table)) {
                int col_idx = table->schema.get_column_index(col_expr->column_name);
                if (col_idx >= 0 && static_cast<u32>(col_idx) < table->col_stats.size()) {
                    const ColumnStats& stats = table->col_stats[col_idx];

                    if (predicate->op == "=") {
                        // Equality: 1/NDV
                        if (stats.ndv > 0) {
                            return 1.0 / static_cast<double>(stats.ndv);
                        }
                        return 0.01;
                    }
                    if (predicate->op == "<>" || predicate->op == "!=") {
                        // Not equal: 1 - 1/NDV
                        if (stats.ndv > 0) {
                            return 1.0 - (1.0 / static_cast<double>(stats.ndv));
                        }
                        return 0.99;
                    }
                    if (predicate->op == ">" || predicate->op == ">=" ||
                        predicate->op == "<" || predicate->op == "<=") {
                        // Range: 1/3 for >, <, >=, <=
                        return 0.33;
                    }
                }
            }

            // Fallback for unindexed/unanalyzed columns
            if (predicate->op == "=") return 0.01;
            if (predicate->op == "<>" || predicate->op == "!=") return 0.99;
            return 0.33;
        }
    }

    // Default
    return 0.1;
}

double Optimizer::sort_cost(double rows) const {
    double log_rows = 1.0;
    double n = rows;
    while (n > 2.0) {
        log_rows += 1.0;
        n = n / 2.0;
    }
    return rows * log_rows * 0.02;
}

UniquePtr<PlanNode> Optimizer::choose_index_path(const SeqScanPlan* scan,
                                                 const Expression* predicate,
                                                 bool whole_predicate,
                                                 bool* predicate_satisfied) {
    if (predicate_satisfied) *predicate_satisfied = false;
    if (!scan || !predicate) return UniquePtr<PlanNode>();

    u32 indexed_col = 0;
    Value low_key;
    Value high_key;
    bool covers_predicate = false;
    bool use_range = false;

    Vector<IndexEntry*> indexes = catalog_ ? catalog_->get_indexes(scan->table_id)
                                           : Vector<IndexEntry*>();
    for (u32 i = 0; i < indexes.size(); i++) {
        IndexEntry* index = indexes[i];
        if (!index || index->state != IndexState::kValid) continue;   // B5/D4: skip invalid / rebuilding
        IndexKey search_key;
        bool covers_full_index = false;
        if (!build_composite_prefix_key(predicate, scan->output_schema,
                                        *index, &search_key, &covers_full_index)) {
            continue;
        }

        auto idx_scan = make_unique<IndexScanPlan>();
        idx_scan->table_id = scan->table_id;
        idx_scan->index_id = index->index_id;
        idx_scan->table_name = scan->table_name;
        idx_scan->search_key = search_key;
        idx_scan->is_range = false;
        idx_scan->output_schema = scan->output_schema;
        (void)covers_full_index;
        if (predicate_satisfied) *predicate_satisfied = false;
        return UniquePtr<PlanNode>(idx_scan.release());
    }

    bool found = find_index_eq_predicate(predicate, scan->output_schema, &indexed_col,
                                         &low_key, &covers_predicate);
    if (!found) {
        found = extract_index_range_predicate(predicate, scan->output_schema, &indexed_col,
                                             &low_key, &high_key, &covers_predicate);
        use_range = found;
        if (!found) {
            bool is_lower = false;
            bool inclusive = false;
            if (extract_index_bound_predicate(predicate, scan->output_schema, &indexed_col,
                                              &low_key, &is_lower, &inclusive)) {
                TableEntry* table = catalog_ ? catalog_->get_table(scan->table_id) : nullptr;
                if (table && indexed_col < table->schema.column_count()) {
                    TypeId type = table->schema.get_column(indexed_col).type;
                    if (is_lower) {
                        high_key = max_key_for_type(type);
                    } else {
                        high_key = low_key;
                        low_key = min_key_for_type(type);
                    }
                    covers_predicate = inclusive;
                    use_range = true;
                    found = true;
                }
            }
        }
    }
    if (!found) return UniquePtr<PlanNode>();

    u32 index_id = 0;
    if (!has_usable_index(scan->table_id, indexed_col, &index_id)) {
        if (predicate->type == ExprType::kBinaryOp && predicate->op == "AND") {
            auto left = choose_index_path(scan, predicate->left.get(), false, predicate_satisfied);
            if (left) return left;
            return choose_index_path(scan, predicate->right.get(), false, predicate_satisfied);
        }
        return UniquePtr<PlanNode>();
    }

    auto idx_scan = make_unique<IndexScanPlan>();
    idx_scan->table_id = scan->table_id;
    idx_scan->index_id = index_id;
    idx_scan->table_name = scan->table_name;
    idx_scan->search_key = IndexKey::single(low_key);
    idx_scan->is_range = use_range;
    idx_scan->range_high = IndexKey::single(high_key);
    idx_scan->output_schema = scan->output_schema;
    if (predicate_satisfied) *predicate_satisfied = whole_predicate && covers_predicate;
    return UniquePtr<PlanNode>(idx_scan.release());
}

bool Optimizer::has_usable_index(u32 table_id, u32 column_idx, u32* index_id) const {
    TableEntry* table = catalog_ ? catalog_->get_table(table_id) : nullptr;
    if (!table || column_idx >= table->schema.column_count() ||
        !btree_supports_type(table->schema.get_column(column_idx).type)) {
        return false;
    }

    Vector<IndexEntry*> indexes = catalog_->get_indexes(table_id);
    for (u32 i = 0; i < indexes.size(); i++) {
        if (indexes[i] && indexes[i]->state == IndexState::kValid &&  // B5/D4: skip invalid
            indexes[i]->key_columns.size() == 1 &&
            indexes[i]->key_columns[0] == column_idx) {
            if (index_id) *index_id = indexes[i]->index_id;
            return true;
        }
    }
    return false;
}

} // namespace minidb
