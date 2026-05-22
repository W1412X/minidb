#include "sql/executor/sort_executor.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unistd.h>

namespace minidb {

SortExecutor::SortExecutor(UniquePtr<Executor> child, Vector<SortKey>&& keys,
                           const Schema& output_schema, u64 work_mem_bytes,
                           i32 top_n, u64 temp_file_limit_bytes,
                           const char* temp_dir)
    : child_(static_cast<UniquePtr<Executor>&&>(child)),
      keys_(static_cast<Vector<SortKey>&&>(keys)), output_schema_(output_schema),
      cursor_(0), materialized_(false), work_mem_bytes_(work_mem_bytes),
      top_n_(top_n), spilled_(false), merge_initialized_(false),
      temp_file_limit_bytes_(temp_file_limit_bytes), temp_bytes_(0),
      temp_dir_(temp_dir && temp_dir[0] ? temp_dir : "/tmp") {}

SortExecutor::~SortExecutor() {
    cleanup_spill_files();
}

void SortExecutor::init() {
    child_->init();
    cursor_ = 0;
    materialized_ = false;
    spilled_ = false;
    merge_initialized_ = false;
    temp_bytes_ = 0;
    buffer_.clear();
    top_heap_.clear();
    cleanup_spill_files();
}

void SortExecutor::materialize() {
    if (materialized_) return;
    if (top_n_ >= 0) {
        materialize_top_n();
        return;
    }
    buffer_.clear();
    u64 memory_used = 0;
    while (true) {
        if (executor_cancelled()) {
            materialized_ = true;
            return;
        }
        ExecResult r = child_->next();
        if (!r.ok()) break;
        memory_used += r.tuple.serialized_size() + 64;
        buffer_.push_back(static_cast<Tuple&&>(r.tuple));
        if (work_mem_bytes_ != 0 && memory_used > work_mem_bytes_) {
            if (!write_sorted_run()) {
                materialized_ = true;
                return;
            }
            spilled_ = true;
            buffer_.clear();
            memory_used = 0;
        }
    }

    if (spilled_) {
        if (buffer_.size() > 0 && !write_sorted_run()) {
            materialized_ = true;
            return;
        }
        buffer_.clear();
        materialized_ = true;
        return;
    }

    // PostgreSQL's tuplesort uses O(n log n) sort path; teaching version uses custom Vector
    // introduce sort instead of O(n^2) insertion sort to avoid ORDER BY degrading at medium data sizes.
    buffer_.sort([this](const Tuple& a, const Tuple& b) {
        return compare_tuples(a, b) < 0;
    });
    materialized_ = true;
}

void SortExecutor::materialize_top_n() {
    buffer_.clear();
    top_heap_.clear();
    if (top_n_ <= 0) {
        materialized_ = true;
        return;
    }

    auto worse_first = [this](const Tuple& a, const Tuple& b) {
        return compare_tuples(a, b) < 0;
    };

    const size_t limit = static_cast<size_t>(top_n_);
    while (true) {
        if (executor_cancelled()) {
            materialized_ = true;
            return;
        }
        ExecResult r = child_->next();
        if (!r.ok()) break;
        Tuple tuple = static_cast<Tuple&&>(r.tuple);
        if (top_heap_.size() < limit) {
            top_heap_.push_back(static_cast<Tuple&&>(tuple));
            std::push_heap(top_heap_.begin(), top_heap_.end(), worse_first);
            continue;
        }
        if (compare_tuples(tuple, top_heap_.front()) < 0) {
            std::pop_heap(top_heap_.begin(), top_heap_.end(), worse_first);
            top_heap_.back() = static_cast<Tuple&&>(tuple);
            std::push_heap(top_heap_.begin(), top_heap_.end(), worse_first);
        }
    }

    for (auto& tuple : top_heap_) {
        buffer_.push_back(static_cast<Tuple&&>(tuple));
    }
    top_heap_.clear();
    buffer_.sort([this](const Tuple& a, const Tuple& b) {
        return compare_tuples(a, b) < 0;
    });
    materialized_ = true;
}

int SortExecutor::compare_tuples(const Tuple& a, const Tuple& b) const {
    for (u32 i = 0; i < keys_.size(); i++) {
        Value va = ExpressionEvaluator::evaluate(*keys_[i].expression, a);
        Value vb = ExpressionEvaluator::evaluate(*keys_[i].expression, b);
        bool a_null = va.is_null();
        bool b_null = vb.is_null();
        if (a_null || b_null) {
            if (a_null && b_null) continue;
            // One is NULL, the other is not.
            bool nf = keys_[i].nulls_first;
            return a_null ? (nf ? -1 : 1) : (nf ? 1 : -1);
        }
        int cmp = va.compare(vb);
        if (cmp != 0) {
            return keys_[i].ascending ? cmp : -cmp;
        }
    }
    return 0;
}

bool SortExecutor::write_sorted_run() {
    buffer_.sort([this](const Tuple& a, const Tuple& b) {
        return compare_tuples(a, b) < 0;
    });

    std::string path_template = temp_dir_;
    if (!path_template.empty() && path_template.back() != '/') path_template += "/";
    path_template += "minidb_sort_XXXXXX";
    std::vector<char> path_buf(path_template.begin(), path_template.end());
    path_buf.push_back('\0');
    int fd = mkstemp(path_buf.data());
    if (fd < 0) {
        set_executor_error("failed to create sort spill file");
        return false;
    }
    std::FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(path_buf.data());
        set_executor_error("failed to open sort spill file");
        return false;
    }

    for (u32 i = 0; i < buffer_.size(); i++) {
        u32 len = buffer_[i].serialized_size();
        u64 write_bytes = static_cast<u64>(sizeof(len)) + len;
        if (temp_file_limit_bytes_ != 0 && temp_bytes_ + write_bytes > temp_file_limit_bytes_) {
            std::fclose(file);
            unlink(path_buf.data());
            set_executor_error("temp_file_limit exceeded during sort");
            return false;
        }
        if (std::fwrite(&len, sizeof(len), 1, file) != 1) {
            std::fclose(file);
            unlink(path_buf.data());
            set_executor_error("failed to write sort spill file");
            return false;
        }
        std::vector<byte> bytes(len);
        buffer_[i].serialize_to_page(bytes.data());
        bool ok = std::fwrite(bytes.data(), 1, len, file) == len;
        if (!ok) {
            std::fclose(file);
            unlink(path_buf.data());
            set_executor_error("failed to write sort spill file");
            return false;
        }
        temp_bytes_ += write_bytes;
    }
    std::fclose(file);
    spill_files_.push_back(path_buf.data());
    return true;
}

bool SortExecutor::read_run_tuple(std::FILE* file, Tuple* out) {
    if (!file || !out) return false;
    u32 len = 0;
    size_t n = std::fread(&len, sizeof(len), 1, file);
    if (n != 1) return false;
    if (len == 0 || len > (64U * 1024U * 1024U)) {
        set_executor_error("corrupt sort spill file");
        return false;
    }
    std::vector<byte> bytes(len);
    bool ok = std::fread(bytes.data(), 1, len, file) == len;
    if (ok) {
        *out = Tuple::deserialize_from_page(bytes.data(), output_schema_, len);
    }
    if (!ok) {
        set_executor_error("failed to read sort spill file");
        return false;
    }
    return true;
}

bool SortExecutor::init_merge() {
    if (merge_initialized_) return true;
    merge_initialized_ = true;
    for (const std::string& path : spill_files_) {
        RunCursor cursor;
        cursor.path = path;
        cursor.file = std::fopen(path.c_str(), "rb");
        if (!cursor.file) {
            set_executor_error("failed to open sort spill file");
            return false;
        }
        cursor.has_tuple = read_run_tuple(cursor.file, &cursor.tuple);
        if (executor_error()) return false;
        if (cursor.has_tuple) {
            run_cursors_.push_back(static_cast<RunCursor&&>(cursor));
        } else {
            std::fclose(cursor.file);
            unlink(cursor.path.c_str());
        }
    }
    return true;
}

void SortExecutor::cleanup_spill_files() {
    for (RunCursor& cursor : run_cursors_) {
        if (cursor.file) {
            std::fclose(cursor.file);
            cursor.file = nullptr;
        }
        if (!cursor.path.empty()) unlink(cursor.path.c_str());
    }
    run_cursors_.clear();
    for (const std::string& path : spill_files_) {
        unlink(path.c_str());
    }
    spill_files_.clear();
}

ExecResult SortExecutor::next() {
    materialize();
    if (executor_error()) return ExecResult::empty();
    if (spilled_) {
        if (!init_merge() || executor_error()) return ExecResult::empty();
        i32 best = -1;
        for (u32 i = 0; i < run_cursors_.size(); i++) {
            if (!run_cursors_[i].has_tuple) continue;
            if (best < 0 || compare_tuples(run_cursors_[i].tuple,
                                           run_cursors_[static_cast<u32>(best)].tuple) < 0) {
                best = static_cast<i32>(i);
            }
        }
        if (best < 0) return ExecResult::empty();
        RunCursor& cursor = run_cursors_[static_cast<u32>(best)];
        Tuple out = static_cast<Tuple&&>(cursor.tuple);
        cursor.has_tuple = read_run_tuple(cursor.file, &cursor.tuple);
        if (!cursor.has_tuple && !executor_error()) {
            if (cursor.file) {
                std::fclose(cursor.file);
                cursor.file = nullptr;
            }
            if (!cursor.path.empty()) unlink(cursor.path.c_str());
        }
        return ExecResult::ok(static_cast<Tuple&&>(out));
    }
    if (cursor_ >= buffer_.size()) return ExecResult::empty();
    return ExecResult::ok(static_cast<Tuple&&>(buffer_[cursor_++]));
}

const Schema& SortExecutor::output_schema() const { return output_schema_; }

} // namespace minidb
