#include "sql/executor/executor_factory.h"
#include "sql/executor/seq_scan.h"
#include "sql/executor/index_scan_executor.h"
#include "sql/executor/filter.h"
#include "sql/executor/project.h"
#include "sql/executor/insert.h"
#include "sql/executor/delete.h"
#include "sql/executor/update.h"
#include "sql/executor/nested_loop_join.h"
#include "sql/executor/hash_join_executor.h"
#include "sql/executor/index_lookup_join.h"
#include "sql/executor/limit_executor.h"
#include "sql/executor/sort_executor.h"
#include "sql/executor/distinct_executor.h"
#include "sql/executor/aggregate_executor.h"
#include "sql/executor/union_executor.h"
#include "sql/executor/subquery_in_executor.h"
#include "sql/executor/trace_report.h"
#include "common/trace.h"
#include "sql/parser/ast.h"
#include "sql/planner/planner.h"
#include "transaction/transaction.h"
#include "concurrency/lock_manager.h"
#include <chrono>

namespace minidb {

ExecutorFactory::ExecutorFactory(Database& db) : db_(db) {}

static OptimizerConfig optimizer_config_from_db(const Database& db) {
    OptimizerConfig cfg;
    cfg.enable_hashjoin = db.config().enable_hashjoin;
    cfg.enable_indexscan = db.config().enable_indexscan;
    cfg.enable_indexonlyscan = db.config().enable_indexonlyscan;
    cfg.remote_storage = db.config().storage_mode == "remote";
    return cfg;
}

class OneRowExecutor : public Executor {
public:
    explicit OneRowExecutor(const Schema& schema) : schema_(schema), emitted_(false) {}
    void init() override { emitted_ = false; }
    ExecResult next() override {
        if (emitted_) return ExecResult::empty();
        emitted_ = true;
        Vector<Value> values;
        return ExecResult::ok(Tuple(schema_, values));
    }
    const Schema& output_schema() const override { return schema_; }

private:
    Schema schema_;
    bool emitted_;
};

class TracedExecutor : public Executor {
public:
    TracedExecutor(UniquePtr<Executor> child, u32 node_id, const char* type_name)
        : child_(static_cast<UniquePtr<Executor>&&>(child)), node_id_(node_id),
          type_name_(type_name ? type_name : "Unknown") {}

    void init() override {
        TraceContext* trace = current_trace();
        TraceNodeScope scope(trace, node_id_);
        auto start = std::chrono::steady_clock::now();
        child_->init();
        auto end = std::chrono::steady_clock::now();
        if (trace) {
            TraceOperatorStats& s = trace->operator_stats(node_id_, type_name_);
            s.init_calls++;
            s.init_us += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        }
    }

    ExecResult next() override {
        TraceContext* trace = current_trace();
        TraceNodeScope scope(trace, node_id_);
        auto start = std::chrono::steady_clock::now();
        ExecResult r = child_->next();
        auto end = std::chrono::steady_clock::now();
        if (trace) {
            TraceOperatorStats& s = trace->operator_stats(node_id_, type_name_);
            s.next_calls++;
            s.next_us += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
            if (r.ok()) s.output_rows++;
        }
        return r;
    }

    const Schema& output_schema() const override { return child_->output_schema(); }
    bool fast_count(u64* count) override { return child_->fast_count(count); }
    bool fast_plain_aggregate(const Vector<AggregateColumn>& aggregates,
                              Vector<Value>* row) override {
        return child_->fast_plain_aggregate(aggregates, row);
    }
    bool last_record_id(RecordId* rid) const override { return child_->last_record_id(rid); }

private:
    UniquePtr<Executor> child_;
    u32 node_id_;
    const char* type_name_;
};

static UniquePtr<Executor> maybe_trace_executor(PlanNode* plan, UniquePtr<Executor> exec) {
    if (!exec || !current_trace() || !plan || plan->trace_id == 0) {
        return static_cast<UniquePtr<Executor>&&>(exec);
    }
    return UniquePtr<Executor>(new TracedExecutor(static_cast<UniquePtr<Executor>&&>(exec),
                                                  plan->trace_id,
                                                  plan_node_type_name(plan->type)));
}

UniquePtr<Executor> ExecutorFactory::create(PlanNode* plan) {
    if (!plan) return UniquePtr<Executor>();
    TraceNodeScope create_scope(current_trace(), plan->trace_id);

    // Get current transaction ID (for locking)
    u64 txn_id = db_.txn_manager().current() ? db_.txn_manager().current()->id() : 0;

    switch (plan->type) {
        case PlanNodeType::kOneRow:
            return maybe_trace_executor(plan, UniquePtr<Executor>(new OneRowExecutor(plan->output_schema)));

        case PlanNodeType::kSeqScan: {
            auto* scan_plan = static_cast<SeqScanPlan*>(plan);
            // Acquire AccessShare lock (read)
            if (txn_id > 0) {
                db_.lock_manager().lock_table(txn_id, scan_plan->table_id,
                                              LockMode::kAccessShare);
            }
            HeapFile* heap = db_.get_heap_file(scan_plan->table_id);
            if (!heap) return UniquePtr<Executor>();
            TableEntry* table = db_.catalog().get_table(scan_plan->table_id);
            const Schema& storage_schema = table ? table->schema : scan_plan->output_schema;
            TransactionManager* tm = db_.txn_manager().current() ? &db_.txn_manager() : nullptr;
            // ParallelSeqScan currently emits all rows up-front into a buffer
            // and has no predicate evaluation, so we only take that path when
            // there's no pushed-down filter.
            if (!tm && db_.config().enable_parallel_seqscan &&
                scan_plan->projected_columns.empty() &&
                !scan_plan->pushed_predicate &&
                db_.config().parallel_workers > 1 &&
                heap->meta().num_data_pages >= db_.config().parallel_workers * 4) {
                return maybe_trace_executor(plan, UniquePtr<Executor>(new ParallelSeqScanExecutor(
                    &db_.pool(), heap, scan_plan->output_schema,
                    db_.config().parallel_workers)));
            }
            auto exec = UniquePtr<SeqScanExecutor>(new SeqScanExecutor(
                &db_.pool(), heap, storage_schema, scan_plan->output_schema,
                scan_plan->projected_columns, tm));
            if (scan_plan->pushed_predicate) {
                auto pred = materialize_scalar_subqueries(scan_plan->pushed_predicate.get());
                if (pred) exec->set_pushed_predicate(static_cast<UniquePtr<Expression>&&>(pred));
            }
            return maybe_trace_executor(plan, UniquePtr<Executor>(exec.release()));
        }

        case PlanNodeType::kIndexScan: {
            auto* scan_plan = static_cast<IndexScanPlan*>(plan);
            HeapFile* heap = db_.get_heap_file(scan_plan->table_id);
            BPlusTree* index = db_.get_index_tree(scan_plan->index_id);
            if (!heap || !index) return UniquePtr<Executor>();
            TransactionManager* tm = db_.txn_manager().current() ? &db_.txn_manager() : nullptr;
            // Pass the index key columns so the scan can recheck stale entries
            // (left by UPDATEs of the indexed column) against the visible tuple.
            IndexEntry* idx_entry = db_.catalog().get_index(scan_plan->index_id);
            Vector<u32> key_cols = idx_entry ? idx_entry->key_columns : Vector<u32>();
            auto exec = UniquePtr<IndexScanExecutor>(new IndexScanExecutor(
                &db_.pool(), heap, index, scan_plan->search_key,
                scan_plan->is_range, scan_plan->range_high,
                scan_plan->output_schema, tm, key_cols));
            if (scan_plan->pushed_predicate) {
                auto pred = materialize_scalar_subqueries(scan_plan->pushed_predicate.get());
                if (pred) exec->set_pushed_predicate(static_cast<UniquePtr<Expression>&&>(pred));
            }
            return maybe_trace_executor(plan, UniquePtr<Executor>(exec.release()));
        }

        case PlanNodeType::kIndexOnlyScan: {
            auto* scan_plan = static_cast<IndexOnlyScanPlan*>(plan);
            BPlusTree* index = db_.get_index_tree(scan_plan->index_id);
            if (!index) return UniquePtr<Executor>();
            TransactionManager* tm = db_.txn_manager().current() ? &db_.txn_manager() : nullptr;
            HeapFile* heap = db_.get_heap_file(scan_plan->table_id);
            // Pass the table schema + single key column so the executor can
            // recheck stale index entries (left behind by UPDATEs of the
            // indexed column) against the visible heap tuple.
            TableEntry* table = db_.catalog().get_table(scan_plan->table_id);
            IndexEntry* idx_entry = db_.catalog().get_index(scan_plan->index_id);
            Schema table_schema = table ? table->schema : Schema();
            u32 key_col = (idx_entry && idx_entry->key_columns.size() == 1)
                              ? idx_entry->key_columns[0]
                              : static_cast<u32>(-1);
            return maybe_trace_executor(plan, UniquePtr<Executor>(new IndexOnlyScanExecutor(
                &db_.pool(), index, scan_plan->search_key, scan_plan->is_range,
                scan_plan->range_high, scan_plan->output_schema, tm, heap,
                table_schema, key_col)));
        }

        case PlanNodeType::kFilter: {
            auto* f_plan = static_cast<FilterPlan*>(plan);
            if (f_plan->predicate &&
                f_plan->predicate->type == ExprType::kBinaryOp &&
                (f_plan->predicate->op == "IN_SUBQUERY" || f_plan->predicate->op == "NOT_IN_SUBQUERY") &&
                f_plan->predicate->right &&
                f_plan->predicate->right->type == ExprType::kSubquery &&
                f_plan->predicate->right->subquery) {
                auto child = create(f_plan->child.get());
                if (!child) return UniquePtr<Executor>();

                Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
                Statement sub_stmt;
                sub_stmt.type = StmtType::kSelect;
                sub_stmt.select = UniquePtr<SelectStmt>(f_plan->predicate->right->subquery->clone());
                UniquePtr<PlanNode> sub_plan = planner.plan(sub_stmt);
                if (!sub_plan) return UniquePtr<Executor>();
                auto sub_exec = create(sub_plan.get());
                if (!sub_exec) return UniquePtr<Executor>();

                return maybe_trace_executor(plan, UniquePtr<Executor>(new SubqueryInExecutor(
                    static_cast<UniquePtr<Executor>&&>(child),
                    static_cast<UniquePtr<Executor>&&>(sub_exec),
                    UniquePtr<Expression>(f_plan->predicate->left->clone()),
                    0, f_plan->output_schema,
                    f_plan->predicate->op == "NOT_IN_SUBQUERY")));
            }
            auto child = create(f_plan->child.get());
            if (!child) return UniquePtr<Executor>();
            auto predicate = materialize_scalar_subqueries(f_plan->predicate.get());
            if (!predicate) return UniquePtr<Executor>();
            return maybe_trace_executor(plan, UniquePtr<Executor>(new FilterExecutor(
                static_cast<UniquePtr<Executor>&&>(child),
                static_cast<UniquePtr<Expression>&&>(predicate))));
        }

        case PlanNodeType::kProject: {
            auto* p_plan = static_cast<ProjectPlan*>(plan);
            auto child = create(p_plan->child.get());
            if (!child) return UniquePtr<Executor>();
            // Materialize scalar subqueries in the projection list (e.g.
            // SELECT (SELECT MAX(v) FROM t)). The expression evaluator cannot
            // execute a subquery node, so without this they evaluated to NULL.
            // Done at build time like the WHERE-clause path; uncorrelated
            // scalar subqueries run once and are substituted as a literal.
            Vector<UniquePtr<Expression>> materialized;
            materialized.reserve(p_plan->expressions.size());
            for (u32 i = 0; i < p_plan->expressions.size(); i++) {
                auto m = materialize_scalar_subqueries(p_plan->expressions[i].get());
                materialized.push_back(m
                    ? static_cast<UniquePtr<Expression>&&>(m)
                    : UniquePtr<Expression>(p_plan->expressions[i]->clone()));
            }
            return maybe_trace_executor(plan, UniquePtr<Executor>(new ProjectExecutor(
                static_cast<UniquePtr<Executor>&&>(child),
                p_plan->output_schema, p_plan->column_indices,
                materialized)));
        }

        case PlanNodeType::kInsert: {
            auto* i_plan = static_cast<InsertPlan*>(plan);
            // Acquire RowExclusive lock (write)
            if (txn_id > 0) {
                db_.lock_manager().lock_table(txn_id, i_plan->table_id,
                                              LockMode::kRowExclusive);
            }
            HeapFile* heap = db_.get_heap_file(i_plan->table_id);
            if (!heap) return UniquePtr<Executor>();
            TransactionManager* tm = db_.txn_manager().current() ? &db_.txn_manager() : nullptr;
            return maybe_trace_executor(plan, UniquePtr<Executor>(new InsertExecutor(
                &db_.pool(), heap, i_plan->output_schema, i_plan->values,
                tm, i_plan->table_id, &db_.wal(), &db_.catalog(), &db_)));
        }

        case PlanNodeType::kDelete: {
            auto* d_plan = static_cast<DeletePlan*>(plan);
            // Acquire RowExclusive lock (write)
            if (txn_id > 0) {
                db_.lock_manager().lock_table(txn_id, d_plan->table_id,
                                              LockMode::kRowExclusive);
            }
            HeapFile* heap = db_.get_heap_file(d_plan->table_id);
            if (!heap) return UniquePtr<Executor>();
            auto child = create(d_plan->child.get());
            if (!child) return UniquePtr<Executor>();
            TransactionManager* tm = db_.txn_manager().current() ? &db_.txn_manager() : nullptr;
            return maybe_trace_executor(plan, UniquePtr<Executor>(new DeleteExecutor(
                &db_.pool(), heap,
                static_cast<UniquePtr<Executor>&&>(child),
                tm, d_plan->table_id, &db_.wal(), &db_)));
        }

        case PlanNodeType::kUpdate: {
            auto* u_plan = static_cast<UpdatePlan*>(plan);
            // Acquire RowExclusive lock (write)
            if (txn_id > 0) {
                db_.lock_manager().lock_table(txn_id, u_plan->table_id,
                                              LockMode::kRowExclusive);
            }
            HeapFile* heap = db_.get_heap_file(u_plan->table_id);
            if (!heap) return UniquePtr<Executor>();
            auto child = create(u_plan->child.get());
            if (!child) return UniquePtr<Executor>();

            Vector<Pair<String, UniquePtr<Expression>>> clauses;
            for (u32 i = 0; i < u_plan->set_clauses.size(); i++) {
                Pair<String, UniquePtr<Expression>> c;
                c.first = u_plan->set_clauses[i].first;
                // Materialize scalar subqueries in the SET value, e.g.
                // UPDATE t SET v = (SELECT MAX(v) FROM t); otherwise the
                // evaluator cannot run the subquery and stores NULL.
                auto m = materialize_scalar_subqueries(u_plan->set_clauses[i].second.get());
                c.second = m ? static_cast<UniquePtr<Expression>&&>(m)
                             : UniquePtr<Expression>(u_plan->set_clauses[i].second->clone());
                clauses.push_back(static_cast<Pair<String, UniquePtr<Expression>>&&>(c));
            }

            TransactionManager* tm = db_.txn_manager().current() ? &db_.txn_manager() : nullptr;
            return maybe_trace_executor(plan, UniquePtr<Executor>(new UpdateExecutor(
                &db_.pool(), heap, u_plan->output_schema,
                static_cast<Vector<Pair<String, UniquePtr<Expression>>>&&>(clauses),
                static_cast<UniquePtr<Executor>&&>(child),
                tm, u_plan->table_id, &db_.wal(), &db_.catalog(), &db_)));
        }

        case PlanNodeType::kJoin: {
            auto* j_plan = static_cast<JoinPlan*>(plan);
            auto left = create(j_plan->left.get());
            auto right = create(j_plan->right.get());
            if (!left || !right) return UniquePtr<Executor>();
            if (j_plan->algorithm == JoinAlgorithm::kIndexLookup &&
                j_plan->lookup_inner_table_id != 0 &&
                j_plan->lookup_inner_index_id != 0 &&
                (j_plan->join_type == JoinType::kInner || j_plan->join_type == JoinType::kLeft)) {
                HeapFile* inner_heap = db_.get_heap_file(j_plan->lookup_inner_table_id);
                BPlusTree* inner_index = db_.get_index_tree(j_plan->lookup_inner_index_id);
                TableEntry* inner_table = db_.catalog().get_table(j_plan->lookup_inner_table_id);
                TransactionManager* tm = db_.txn_manager().current() ? &db_.txn_manager() : nullptr;
                if (inner_heap && inner_index && inner_table) {
                    Vector<u32> inner_projected;
                    if (j_plan->right && j_plan->right->type == PlanNodeType::kSeqScan) {
                        inner_projected = static_cast<SeqScanPlan*>(j_plan->right.get())->projected_columns;
                    }
                    return maybe_trace_executor(plan, UniquePtr<Executor>(new IndexLookupJoinExecutor(
                        static_cast<UniquePtr<Executor>&&>(left),
                        &db_.pool(), inner_heap, inner_index,
                        inner_table->schema, j_plan->right->output_schema, inner_projected,
                        j_plan->on_condition ? UniquePtr<Expression>(j_plan->on_condition->clone()) : UniquePtr<Expression>(),
                        j_plan->output_schema, j_plan->join_type, tm)));
                }
            }
            if (j_plan->algorithm == JoinAlgorithm::kHash &&
                (j_plan->join_type == JoinType::kInner || j_plan->join_type == JoinType::kLeft) &&
                j_plan->on_condition &&
                j_plan->on_condition->type == ExprType::kBinaryOp &&
                j_plan->on_condition->op == "=") {
                return maybe_trace_executor(plan, UniquePtr<Executor>(new HashJoinExecutor(
                    static_cast<UniquePtr<Executor>&&>(left),
                    static_cast<UniquePtr<Executor>&&>(right),
                    UniquePtr<Expression>(j_plan->on_condition->clone()),
                    j_plan->output_schema, j_plan->join_type,
                    db_.config().work_mem_bytes, j_plan->hash_build_left,
                    db_.config().temp_dir.c_str())));
            }
            return maybe_trace_executor(plan, UniquePtr<Executor>(new NestedLoopJoinExecutor(
                static_cast<UniquePtr<Executor>&&>(left),
                static_cast<UniquePtr<Executor>&&>(right),
                j_plan->on_condition ? UniquePtr<Expression>(j_plan->on_condition->clone()) : UniquePtr<Expression>(),
                j_plan->output_schema, j_plan->join_type)));
        }

        case PlanNodeType::kLimit: {
            auto* l_plan = static_cast<LimitPlan*>(plan);
            auto child = create(l_plan->child.get());
            if (!child) return UniquePtr<Executor>();
            return maybe_trace_executor(plan, UniquePtr<Executor>(new LimitExecutor(
                static_cast<UniquePtr<Executor>&&>(child),
                l_plan->limit, l_plan->offset)));
        }

        case PlanNodeType::kSort: {
            auto* s_plan = static_cast<SortPlan*>(plan);
            auto child = create(s_plan->child.get());
            if (!child) return UniquePtr<Executor>();
            Vector<SortKey> keys;
            for (u32 i = 0; i < s_plan->keys.size(); i++) {
                SortKey sk;
                sk.expression = UniquePtr<Expression>(s_plan->keys[i].expression->clone());
                sk.ascending = s_plan->keys[i].ascending;
                sk.nulls_first = s_plan->keys[i].nulls_first;
                keys.push_back(static_cast<SortKey&&>(sk));
            }
            return maybe_trace_executor(plan, UniquePtr<Executor>(new SortExecutor(
                static_cast<UniquePtr<Executor>&&>(child),
                static_cast<Vector<SortKey>&&>(keys), s_plan->output_schema,
                db_.config().work_mem_bytes, s_plan->top_n,
                db_.config().temp_file_limit_bytes, db_.config().temp_dir.c_str())));
        }

        case PlanNodeType::kDistinct: {
            auto* dist_plan = static_cast<DistinctPlan*>(plan);
            auto child = create(dist_plan->child.get());
            if (!child) return UniquePtr<Executor>();
            return maybe_trace_executor(plan, UniquePtr<Executor>(new DistinctExecutor(
                static_cast<UniquePtr<Executor>&&>(child), dist_plan->output_schema,
                db_.config().work_mem_bytes, db_.config().temp_dir.c_str())));
        }

        case PlanNodeType::kAggregate: {
            auto* a_plan = static_cast<AggregatePlan*>(plan);
            auto child = create(a_plan->child.get());
            if (!child) return UniquePtr<Executor>();

            Vector<AggregateColumn> aggs;
            for (u32 i = 0; i < a_plan->aggregates.size(); i++) {
                AggregateColumn ac;
                ac.func = a_plan->aggregates[i].func;
                ac.distinct = a_plan->aggregates[i].distinct;
                if (a_plan->aggregates[i].argument)
                    ac.argument = UniquePtr<Expression>(a_plan->aggregates[i].argument->clone());
                aggs.push_back(static_cast<AggregateColumn&&>(ac));
            }

            Vector<UniquePtr<Expression>> gb;
            for (u32 i = 0; i < a_plan->group_by.size(); i++) {
                gb.push_back(UniquePtr<Expression>(a_plan->group_by[i]->clone()));
            }

            UniquePtr<Expression> having;
            if (a_plan->having)
                having = UniquePtr<Expression>(a_plan->having->clone());

            return maybe_trace_executor(plan, UniquePtr<Executor>(new AggregateExecutor(
                static_cast<UniquePtr<Executor>&&>(child),
                static_cast<Vector<AggregateColumn>&&>(aggs),
                static_cast<Vector<UniquePtr<Expression>>&&>(gb),
                static_cast<UniquePtr<Expression>&&>(having),
                a_plan->output_schema,
                db_.config().work_mem_bytes,
                db_.config().temp_dir.c_str())));
        }

        case PlanNodeType::kUnion: {
            auto* u_plan = static_cast<UnionPlan*>(plan);
            auto left = create(u_plan->left.get());
            auto right = create(u_plan->right.get());
            if (!left || !right) return UniquePtr<Executor>();
            return maybe_trace_executor(plan, UniquePtr<Executor>(new UnionExecutor(
                static_cast<UniquePtr<Executor>&&>(left),
                static_cast<UniquePtr<Executor>&&>(right),
                u_plan->all, u_plan->output_schema)));
        }

        default:
            return UniquePtr<Executor>();
    }
}

Value ExecutorFactory::evaluate_scalar_subquery(const SelectStmt* subquery) {
    if (!subquery) return Value();

    Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
    Statement stmt;
    stmt.type = StmtType::kSelect;
    stmt.select = UniquePtr<SelectStmt>(subquery->clone());

    UniquePtr<PlanNode> plan = planner.plan(stmt);
    if (!plan) return Value();

    auto exec = create(plan.get());
    if (!exec) return Value();

    exec->init();
    ExecResult row = exec->next();
    if (!row.ok() || exec->output_schema().column_count() == 0) return Value();
    return row.tuple.get_value(0);
}

UniquePtr<Expression> ExecutorFactory::materialize_scalar_subqueries(const Expression* expr) {
    if (!expr) return UniquePtr<Expression>();

    if (expr->type == ExprType::kSubquery) {
        auto lit = make_unique<Expression>();
        lit->type = ExprType::kLiteral;
        lit->literal_value = evaluate_scalar_subquery(expr->subquery.get());
        return UniquePtr<Expression>(lit.release());
    }

    auto copy = make_unique<Expression>();
    copy->type = expr->type;
    copy->table_name = expr->table_name;
    copy->column_name = expr->column_name;
    copy->literal_value = expr->literal_value;
    copy->alias = expr->alias;
    copy->op = expr->op;
    copy->cast_target = expr->cast_target;
    if (expr->left) copy->left = materialize_scalar_subqueries(expr->left.get());
    if (expr->right) copy->right = materialize_scalar_subqueries(expr->right.get());
    if (expr->child) copy->child = materialize_scalar_subqueries(expr->child.get());
    if (expr->subquery) copy->subquery = UniquePtr<SelectStmt>(expr->subquery->clone());
    for (u32 i = 0; i < expr->when_clauses.size(); i++) {
        Pair<UniquePtr<Expression>, UniquePtr<Expression>> clause;
        clause.first = materialize_scalar_subqueries(expr->when_clauses[i].first.get());
        clause.second = materialize_scalar_subqueries(expr->when_clauses[i].second.get());
        copy->when_clauses.push_back(static_cast<Pair<UniquePtr<Expression>, UniquePtr<Expression>>&&>(clause));
    }
    if (expr->else_expr) copy->else_expr = materialize_scalar_subqueries(expr->else_expr.get());
    return UniquePtr<Expression>(copy.release());
}

} // namespace minidb
