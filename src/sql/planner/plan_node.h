/**
 * @file plan_node.h
 * @brief Plan node — Logical query plan tree
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include "container/vector.h"
#include "container/unique_ptr.h"
#include "container/utility.h"
#include "index/index_key.h"
#include "record/schema.h"
#include "record/value.h"

namespace minidb {

enum class PlanNodeType {
    kOneRow, kSeqScan, kInsert, kDelete, kUpdate, kFilter, kProject, kJoin,
    kLimit, kSort, kDistinct, kAggregate, kUnion, kIndexScan, kIndexOnlyScan,
};

// Forward declarations.
struct Expression;

enum class JoinType { kInner, kLeft };

enum class JoinAlgorithm { kNestedLoop, kHash, kIndexLookup, kMerge };

enum class AggFunc { kCount, kSum, kAvg, kMin, kMax };

struct PlanNode {
    PlanNodeType type;
    Schema output_schema;
    double startup_cost = 0.0;
    double total_cost = 0.0;
    double plan_rows = 0.0;
    String optimizer_note;
    virtual ~PlanNode() = default;
};

struct OneRowPlan : PlanNode {
    OneRowPlan() { type = PlanNodeType::kOneRow; }
};

struct SeqScanPlan : PlanNode {
    u32 table_id;
    String table_name;
    Vector<u32> projected_columns;
    // Optional pushed-down WHERE predicate. When set, the scan evaluates the
    // predicate inline on each visible tuple and only yields tuples whose
    // predicate is true. Logically a `Filter(SeqScan(t))` is rewritten by
    // the planner into `SeqScan(t, pred)` so that the executor's per-row
    // virtual-call + result-move overhead is amortized to zero — and so
    // rows that fail the filter never have to construct an ExecResult.
    UniquePtr<Expression> pushed_predicate;
    SeqScanPlan() { type = PlanNodeType::kSeqScan; }
};

struct IndexScanPlan : PlanNode {
    u32 table_id;
    u32 index_id;
    String table_name;
    IndexKey search_key;
    bool is_range;
    IndexKey range_high;
    // Residual predicate left over after the index range constrained the
    // search key. Evaluated inline against each candidate heap tuple before
    // emission, so rows failing it don't pay the result-move cost.
    UniquePtr<Expression> pushed_predicate;
    IndexScanPlan() : table_id(0), index_id(0), is_range(false) {
        type = PlanNodeType::kIndexScan;
    }
};

struct IndexOnlyScanPlan : PlanNode {
    u32 table_id;
    u32 index_id;
    String table_name;
    IndexKey search_key;
    bool is_range;
    IndexKey range_high;
    IndexOnlyScanPlan() : table_id(0), index_id(0), is_range(false) {
        type = PlanNodeType::kIndexOnlyScan;
    }
};

struct InsertPlan : PlanNode {
    u32 table_id;
    Vector<Vector<Value>> values;
    InsertPlan() { type = PlanNodeType::kInsert; }
};

struct DeletePlan : PlanNode {
    u32 table_id;
    UniquePtr<PlanNode> child;
    DeletePlan() { type = PlanNodeType::kDelete; }
};

struct UpdatePlan : PlanNode {
    u32 table_id;
    Vector<Pair<String, UniquePtr<Expression>>> set_clauses;
    UniquePtr<Expression> where_clause;
    UniquePtr<PlanNode> child;
    UpdatePlan() { type = PlanNodeType::kUpdate; }
};

struct FilterPlan : PlanNode {
    UniquePtr<Expression> predicate;
    UniquePtr<PlanNode> child;
    FilterPlan() { type = PlanNodeType::kFilter; }
};

struct ProjectPlan : PlanNode {
    Vector<u32> column_indices;
    Vector<UniquePtr<Expression>> expressions;
    UniquePtr<PlanNode> child;
    ProjectPlan() { type = PlanNodeType::kProject; }
};

struct JoinPlan : PlanNode {
    UniquePtr<PlanNode> left;
    UniquePtr<PlanNode> right;
    UniquePtr<Expression> on_condition;
    JoinType join_type;
    JoinAlgorithm algorithm;
    bool hash_build_left;
    u32 lookup_inner_table_id;
    u32 lookup_inner_index_id;
    double hint_weight;  // Join hint: >0 favors hash, <0 favors nested (cost multiplier)
    JoinPlan() : join_type(JoinType::kInner), algorithm(JoinAlgorithm::kNestedLoop),
                 hash_build_left(false), lookup_inner_table_id(0), lookup_inner_index_id(0),
                 hint_weight(0.0) {
        type = PlanNodeType::kJoin;
    }
};

struct LimitPlan : PlanNode {
    i32 limit;
    i32 offset;
    UniquePtr<PlanNode> child;
    LimitPlan() : limit(-1), offset(-1) { type = PlanNodeType::kLimit; }
};

struct SortKey {
    UniquePtr<Expression> expression;
    bool ascending;
    bool nulls_first;   // true = NULLs sort before non-NULLs
    SortKey() : ascending(true), nulls_first(true) {}
};

struct SortPlan : PlanNode {
    Vector<SortKey> keys;
    UniquePtr<PlanNode> child;
    i32 top_n;
    SortPlan() : top_n(-1) { type = PlanNodeType::kSort; }
};

struct DistinctPlan : PlanNode {
    UniquePtr<PlanNode> child;
    DistinctPlan() { type = PlanNodeType::kDistinct; }
};

struct AggregateColumn {
    AggFunc func;
    UniquePtr<Expression> argument;
    String alias;
    bool distinct;
    AggregateColumn() : func(AggFunc::kCount), distinct(false) {}
};

struct AggregatePlan : PlanNode {
    Vector<AggregateColumn> aggregates;
    Vector<UniquePtr<Expression>> group_by;
    UniquePtr<Expression> having;
    UniquePtr<PlanNode> child;
    AggregatePlan() { type = PlanNodeType::kAggregate; }
};

struct UnionPlan : PlanNode {
    UniquePtr<PlanNode> left;
    UniquePtr<PlanNode> right;
    bool all;
    UnionPlan() : all(false) { type = PlanNodeType::kUnion; }
};

} // namespace minidb
