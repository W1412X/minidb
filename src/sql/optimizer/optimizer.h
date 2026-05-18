/**
 * @file optimizer.h
 * @brief PostgreSQL-style rule/cost optimizer for the teaching database.
 */
#pragma once

#include "catalog/catalog.h"
#include "container/unique_ptr.h"
#include "sql/planner/plan_node.h"

namespace minidb {

struct OptimizerConfig {
    bool enable_seqscan = true;
    bool enable_indexscan = true;
    bool enable_indexonlyscan = true;
    bool enable_hashjoin = true;
    bool remote_storage = false;
    double local_seq_page_cost = 0.03;
    double local_random_page_cost = 0.08;
    double remote_seq_page_cost = 0.20;
    double remote_random_page_cost = 0.65;
    double remote_round_trip_cost = 0.50;
};

class Optimizer {
public:
    explicit Optimizer(Catalog* catalog);
    Optimizer(Catalog* catalog, const OptimizerConfig& config);

    UniquePtr<PlanNode> optimize(UniquePtr<PlanNode> plan);

private:
    UniquePtr<PlanNode> optimize_node(UniquePtr<PlanNode> plan);
    UniquePtr<PlanNode> optimize_filter(UniquePtr<FilterPlan> filter);
    void estimate_scan(PlanNode* plan);
    void estimate_unary(PlanNode* plan, const PlanNode* child, double selectivity, double cpu);
    void estimate_binary(PlanNode* plan, const PlanNode* left, const PlanNode* right, double selectivity);
    double estimate_table_rows(u32 table_id) const;
    double sort_cost(double rows) const;
    double estimate_selectivity(const Expression* predicate, u32 table_id) const;

    UniquePtr<PlanNode> choose_index_path(const SeqScanPlan* scan,
                                          const Expression* predicate,
                                          bool whole_predicate,
                                          bool* predicate_satisfied);
    bool has_usable_index(u32 table_id, u32 column_idx, u32* index_id) const;

    Catalog* catalog_;
    OptimizerConfig config_;
};

} // namespace minidb
