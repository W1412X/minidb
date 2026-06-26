/**
 * @file plan_node.cpp
 * @brief Out-of-line destructors for plan nodes that own UniquePtr<Expression>.
 *
 * Expression is forward-declared in plan_node.h, so any plan node holding a
 * UniquePtr<Expression> must have its destructor emitted in a translation unit
 * where Expression is a complete type. Otherwise the UniquePtr destructor would
 * `delete` a pointer to an incomplete type — undefined behavior that silently
 * skips Expression's (non-trivial) destructor and leaks every child expression.
 */
#include "sql/parser/ast.h"      // complete definition of Expression
#include "sql/planner/plan_node.h"

namespace minidb {

// Constructors are also defined here (not just destructors): an inline
// constructor instantiates the member UniquePtr<Expression> destructor for its
// exception-cleanup path, which would again require Expression to be complete.

SeqScanPlan::SeqScanPlan() { type = PlanNodeType::kSeqScan; }
SeqScanPlan::~SeqScanPlan() = default;

IndexScanPlan::IndexScanPlan() : table_id(0), index_id(0), is_range(false) {
    type = PlanNodeType::kIndexScan;
}
IndexScanPlan::~IndexScanPlan() = default;

UpdatePlan::UpdatePlan() { type = PlanNodeType::kUpdate; }
UpdatePlan::~UpdatePlan() = default;

FilterPlan::FilterPlan() { type = PlanNodeType::kFilter; }
FilterPlan::~FilterPlan() = default;

ProjectPlan::ProjectPlan() { type = PlanNodeType::kProject; }
ProjectPlan::~ProjectPlan() = default;

JoinPlan::JoinPlan()
    : join_type(JoinType::kInner), algorithm(JoinAlgorithm::kNestedLoop),
      hash_build_left(false), lookup_inner_table_id(0), lookup_inner_index_id(0),
      hint_weight(0.0) {
    type = PlanNodeType::kJoin;
}
JoinPlan::~JoinPlan() = default;

SortKey::SortKey() : ascending(true), nulls_first(true) {}
SortKey::~SortKey() = default;
SortKey::SortKey(SortKey&&) noexcept = default;
SortKey& SortKey::operator=(SortKey&&) noexcept = default;

AggregateColumn::AggregateColumn() : func(AggFunc::kCount), distinct(false) {}
AggregateColumn::~AggregateColumn() = default;
AggregateColumn::AggregateColumn(AggregateColumn&&) noexcept = default;
AggregateColumn& AggregateColumn::operator=(AggregateColumn&&) noexcept = default;

SortPlan::SortPlan() : top_n(-1) { type = PlanNodeType::kSort; }
SortPlan::~SortPlan() = default;

AggregatePlan::AggregatePlan() { type = PlanNodeType::kAggregate; }
AggregatePlan::~AggregatePlan() = default;

} // namespace minidb
