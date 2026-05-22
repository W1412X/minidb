/**
 * @file hash_join_executor.h
 * @brief Hash Join executor — optimization for equi-join
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "container/vector.h"
#include "container/hash_map.h"
#include "record/value.h"
#include "sql/planner/plan_node.h"
#include <cstdio>
#include <string>

namespace minidb {

struct Expression;

struct HashJoinEntry {
    Tuple tuple;
    HashJoinEntry* next;
    HashJoinEntry() : next(nullptr) {}
};

class HashJoinExecutor : public Executor {
public:
    HashJoinExecutor(UniquePtr<Executor> left, UniquePtr<Executor> right,
                     UniquePtr<Expression> on_condition,
                     const Schema& output_schema,
                     JoinType join_type = JoinType::kInner,
                     u64 work_mem_bytes = 0,
                     bool build_left = false,
                     const char* temp_dir = "/tmp");
    ~HashJoinExecutor() override;
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;
    bool fast_count(u64* count) override;

private:
    void build_hash_table();
    Value eval_key(Expression* expr, const Tuple& tuple);
    u64 hash_value(const Value& v);
    bool can_evaluate_on(Expression* expr, const Schema& schema) const;
    bool write_spill_tuple(std::FILE* file, const Tuple& tuple);
    bool read_spill_tuple(std::FILE* file, const Schema& schema, Tuple* out);
    void cleanup_spill();
    bool prepare_grace_partitions();
    bool load_next_partition();
    ExecResult next_spilled();
    ExecResult make_join_tuple(const Tuple& probe, const Tuple& build);

    UniquePtr<Executor> left_;
    UniquePtr<Executor> right_;
    UniquePtr<Expression> on_condition_;
    Schema output_schema_;

    // Hash Map: key hash → Linked List
    static constexpr u32 BUCKET_COUNT = 8192;
    HashJoinEntry* buckets_[BUCKET_COUNT];
    Vector<HashJoinEntry*> all_entries_;  // owns entry storage
    Tuple current_left_;
    HashJoinEntry* current_entry_;
    Expression* left_key_expr_;
    Expression* right_key_expr_;
    bool has_current_left_;
    bool current_left_matched_;
    JoinType join_type_;
    bool build_left_;
    bool built_;
    u64 work_mem_bytes_;
    bool spilled_;
    std::string right_spill_path_;
    std::string temp_dir_;
    std::FILE* right_scan_;
    static constexpr u32 GRACE_PARTITIONS = 32;
    std::string build_partitions_[GRACE_PARTITIONS];
    std::string probe_partitions_[GRACE_PARTITIONS];
    std::FILE* probe_scan_;
    u32 current_partition_;
    bool grace_ready_;
};

} // namespace minidb
