/**
 * @file index_lookup_join.h
 * @brief Index nested-loop join executor for indexed equi-joins.
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "sql/planner/plan_node.h"

namespace minidb {

struct Expression;
class TransactionManager;
class HeapFile;

class IndexLookupJoinExecutor : public Executor {
public:
    IndexLookupJoinExecutor(UniquePtr<Executor> outer,
                            BufferPool* pool,
                            HeapFile* inner_heap,
                            BPlusTree* inner_index,
                            const Schema& inner_storage_schema,
                            const Schema& inner_output_schema,
                            const Vector<u32>& inner_projected_columns,
                            UniquePtr<Expression> on_condition,
                            const Schema& output_schema,
                            JoinType join_type,
                            TransactionManager* txn_mgr);

    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;
    bool fast_count(u64* count) override;

private:
    bool can_evaluate_on(Expression* expr, const Schema& schema) const;
    Value eval_key(Expression* expr, const Tuple& tuple);
    bool setup_keys();
    bool fetch_inner(const RecordId& rid, Tuple* tuple);
    ExecResult make_join_tuple(const Tuple& outer, const Tuple& inner);
    ExecResult make_outer_only_tuple(const Tuple& outer);

    UniquePtr<Executor> outer_;
    BufferPool* pool_;
    HeapFile* inner_heap_;
    BPlusTree* inner_index_;
    Schema inner_storage_schema_;
    Schema inner_output_schema_;
    Vector<u32> inner_projected_columns_;
    UniquePtr<Expression> on_condition_;
    Schema output_schema_;
    JoinType join_type_;
    TransactionManager* txn_mgr_;

    Expression* outer_key_expr_;
    Expression* inner_key_expr_;
    Tuple current_outer_;
    Value current_key_;
    PageId scan_leaf_id_;
    u16 scan_slot_idx_;
    RecordId last_index_rid_;
    bool has_last_index_rid_;
    bool has_current_outer_;
    bool current_outer_matched_;
};

} // namespace minidb
