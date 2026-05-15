/**
 * @file sort_executor.h
 * @brief ORDER BY Sort executor — materialize all rows then sort and output
 */
#pragma once

#include "sql/executor/executor.h"
#include "sql/planner/plan_node.h"
#include "container/unique_ptr.h"
#include "container/vector.h"
#include <cstdio>
#include <string>
#include <vector>

namespace minidb {

class SortExecutor : public Executor {
public:
    SortExecutor(UniquePtr<Executor> child, Vector<SortKey>&& keys,
                 const Schema& output_schema, u64 work_mem_bytes = 0,
                 i32 top_n = -1, u64 temp_file_limit_bytes = 0,
                 const char* temp_dir = "/tmp");
    ~SortExecutor() override;
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    struct RunCursor {
        std::FILE* file = nullptr;
        std::string path;
        Tuple tuple;
        bool has_tuple = false;
    };

    void materialize();
    int compare_tuples(const Tuple& a, const Tuple& b) const;
    bool write_sorted_run();
    bool read_run_tuple(std::FILE* file, Tuple* out);
    bool init_merge();
    void cleanup_spill_files();
    void materialize_top_n();

    UniquePtr<Executor> child_;
    Vector<SortKey> keys_;
    Schema output_schema_;
    Vector<Tuple> buffer_;
    std::vector<Tuple> top_heap_;
    u32 cursor_;
    bool materialized_;
    u64 work_mem_bytes_;
    i32 top_n_;
    bool spilled_;
    bool merge_initialized_;
    u64 temp_file_limit_bytes_;
    u64 temp_bytes_;
    std::string temp_dir_;
    std::vector<std::string> spill_files_;
    std::vector<RunCursor> run_cursors_;
};

} // namespace minidb
