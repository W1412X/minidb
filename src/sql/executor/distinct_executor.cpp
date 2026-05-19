#include "sql/executor/distinct_executor.h"
#include "common/tuple_key.h"
#include "container/hash_map.h"
#include <cstdio>
#include <string>
#include <vector>
#include <unistd.h>

namespace minidb {

DistinctExecutor::DistinctExecutor(UniquePtr<Executor> child, const Schema& output_schema,
                                   u64 work_mem_bytes,
                                   const char* temp_dir)
    : child_(static_cast<UniquePtr<Executor>&&>(child)),
      output_schema_(output_schema), cursor_(0), work_mem_bytes_(work_mem_bytes),
      temp_dir_(temp_dir && temp_dir[0] ? temp_dir : "/tmp") {}

void DistinctExecutor::init() {
    child_->init();
    seen_.clear();
    cursor_ = 0;
    HashMap<String, bool> keys;
    u64 memory_used = 0;
    std::vector<std::string> run_paths;
    Vector<Tuple> chunk;
    const Schema& schema = child_->output_schema();

    auto cleanup = [&]() {
        for (const std::string& path : run_paths) unlink(path.c_str());
    };
    auto write_run = [&]() -> bool {
        if (chunk.size() == 0) return true;
        chunk.sort([](const Tuple& a, const Tuple& b) {
            return make_tuple_key(a) < make_tuple_key(b);
        });
        std::string path_template = temp_dir_;
        if (!path_template.empty() && path_template.back() != '/') path_template += "/";
        path_template += "minidb_distinct_XXXXXX";
        std::vector<char> path(path_template.begin(), path_template.end());
        path.push_back('\0');
        int fd = mkstemp(path.data());
        if (fd < 0) {
            set_executor_error("failed to create distinct spill file");
            return false;
        }
        std::FILE* file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
            unlink(path.data());
            set_executor_error("failed to open distinct spill file");
            return false;
        }
        for (u32 i = 0; i < chunk.size(); i++) {
            u32 len = chunk[i].serialized_size();
            if (std::fwrite(&len, sizeof(len), 1, file) != 1) {
                std::fclose(file);
                unlink(path.data());
                set_executor_error("failed to write distinct spill file");
                return false;
            }
            std::vector<byte> bytes(len);
            chunk[i].serialize_to_page(bytes.data());
            bool ok = std::fwrite(bytes.data(), 1, len, file) == len;
            if (!ok) {
                std::fclose(file);
                unlink(path.data());
                set_executor_error("failed to write distinct spill file");
                return false;
            }
        }
        std::fclose(file);
        run_paths.push_back(path.data());
        chunk.clear();
        memory_used = 0;
        return true;
    };

    while (true) {
        if (executor_cancelled()) {
            cleanup();
            return;
        }
        ExecResult r = child_->next();
        if (!r.ok()) break;
        Tuple t = static_cast<Tuple&&>(r.tuple);
        String key = make_tuple_key(t);
        if (run_paths.empty() && !keys.find(key)) {
            memory_used += t.serialized_size() + key.size() + 96;
            if (work_mem_bytes_ != 0 && memory_used > work_mem_bytes_) {
                for (u32 i = 0; i < seen_.size(); i++) {
                    chunk.push_back(static_cast<Tuple&&>(seen_[i]));
                }
                seen_.clear();
                keys.clear();
                chunk.push_back(static_cast<Tuple&&>(t));
                if (!write_run()) {
                    cleanup();
                    return;
                }
                continue;
            }
            keys.insert(key, true);
            seen_.push_back(static_cast<Tuple&&>(t));
        } else if (!run_paths.empty()) {
            memory_used += t.serialized_size() + key.size() + 96;
            chunk.push_back(static_cast<Tuple&&>(t));
            if (work_mem_bytes_ != 0 && memory_used > work_mem_bytes_) {
                if (!write_run()) {
                    cleanup();
                    return;
                }
            }
        }
    }

    if (run_paths.empty()) return;
    if (!write_run()) {
        cleanup();
        return;
    }

    struct RunCursor {
        std::FILE* file = nullptr;
        std::string path;
        Tuple tuple;
        String key;
        bool has_tuple = false;
    };
    auto read_tuple = [&](std::FILE* file, Tuple* out) -> bool {
        u32 len = 0;
        if (std::fread(&len, sizeof(len), 1, file) != 1) return false;
        if (len == 0 || len > 64U * 1024U * 1024U) {
            set_executor_error("corrupt distinct spill file");
            return false;
        }
        std::vector<byte> bytes(len);
        bool ok = std::fread(bytes.data(), 1, len, file) == len;
        if (ok) *out = Tuple::deserialize_from_page(bytes.data(), schema, len);
        if (!ok) set_executor_error("failed to read distinct spill file");
        return ok;
    };

    std::vector<RunCursor> cursors;
    auto close_cursors = [&]() {
        for (RunCursor& c : cursors) {
            if (c.file) {
                std::fclose(c.file);
                c.file = nullptr;
            }
        }
    };
    for (const std::string& path : run_paths) {
        RunCursor c;
        c.path = path;
        c.file = std::fopen(path.c_str(), "rb");
        if (!c.file) {
            set_executor_error("failed to open distinct spill file");
            close_cursors();
            cleanup();
            return;
        }
        c.has_tuple = read_tuple(c.file, &c.tuple);
        if (executor_error()) {
            close_cursors();
            cleanup();
            return;
        }
        if (c.has_tuple) c.key = make_tuple_key(c.tuple);
        cursors.push_back(static_cast<RunCursor&&>(c));
    }

    String last_key;
    bool have_last = false;
    while (true) {
        if (executor_cancelled()) break;
        i32 best = -1;
        for (u32 i = 0; i < cursors.size(); i++) {
            if (!cursors[i].has_tuple) continue;
            if (best < 0 || cursors[i].key < cursors[static_cast<u32>(best)].key) {
                best = static_cast<i32>(i);
            }
        }
        if (best < 0) break;
        RunCursor& c = cursors[static_cast<u32>(best)];
        if (!have_last || c.key != last_key) {
            seen_.push_back(c.tuple);
            last_key = c.key;
            have_last = true;
        }
        c.has_tuple = read_tuple(c.file, &c.tuple);
        if (executor_error()) break;
        if (c.has_tuple) c.key = make_tuple_key(c.tuple);
    }
    close_cursors();
    cleanup();
}

bool DistinctExecutor::tuples_equal(const Tuple& a, const Tuple& b) const {
    if (a.column_count() != b.column_count()) return false;
    for (u32 i = 0; i < a.column_count(); i++) {
        if (a.get_value(i) != b.get_value(i)) return false;
    }
    return true;
}

ExecResult DistinctExecutor::next() {
    if (executor_error()) return ExecResult::empty();
    if (cursor_ >= seen_.size()) return ExecResult::empty();
    return ExecResult::ok(static_cast<Tuple&&>(seen_[cursor_++]));
}

const Schema& DistinctExecutor::output_schema() const { return output_schema_; }

} // namespace minidb
