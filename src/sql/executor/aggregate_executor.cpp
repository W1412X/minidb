#include "sql/executor/aggregate_executor.h"
#include "common/tuple_key.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"
#include <cstdio>
#include <string>
#include <vector>
#include <unistd.h>

namespace minidb {

AggregateExecutor::AggregateExecutor(
    UniquePtr<Executor> child,
    Vector<AggregateColumn>&& aggregates,
    Vector<UniquePtr<Expression>>&& group_by,
    UniquePtr<Expression> having,
    const Schema& output_schema,
    u64 work_mem_bytes,
    const char* temp_dir)
    : child_(static_cast<UniquePtr<Executor>&&>(child)),
      aggregates_(static_cast<Vector<AggregateColumn>&&>(aggregates)),
      having_(static_cast<UniquePtr<Expression>&&>(having)),
      output_schema_(output_schema), cursor_(0), work_mem_bytes_(work_mem_bytes),
      temp_dir_(temp_dir && temp_dir[0] ? temp_dir : "/tmp") {
    for (u32 i = 0; i < group_by.size(); i++) {
        group_by_.push_back(UniquePtr<Expression>(group_by[i]->clone()));
    }
}

void AggregateExecutor::init() {
    child_->init();
    cursor_ = 0;
    result_groups_.clear();
    compute_groups();
}

struct AggState {
    u64 count;
    Value value;
    bool has_value;
    HashMap<String, bool> seen;

    AggState() : count(0), value(), has_value(false) {}
};

static double numeric_value_as_double(const Value& value, bool* ok) {
    if (ok) *ok = true;
    switch (value.type_id()) {
        case TypeId::kBool:
            return value.get_bool() ? 1.0 : 0.0;
        case TypeId::kInt32:
            return static_cast<double>(value.get_int32());
        case TypeId::kInt64:
            return static_cast<double>(value.get_int64());
        case TypeId::kFloat:
            return static_cast<double>(value.get_float());
        case TypeId::kDouble:
            return value.get_double();
        default:
            if (ok) *ok = false;
            return 0.0;
    }
}

static void advance_agg(AggState* state, AggFunc func, const Value& input, bool distinct = false) {
    if (!state) return;
    if (distinct) {
        Vector<Value> key_values;
        key_values.push_back(input);
        String key = make_values_key(key_values);
        if (state->seen.find(key)) return;
        state->seen.insert(key, true);
    }
    if (func == AggFunc::kCount) {
        state->count++;
        state->has_value = true;
        return;
    }
    if (input.is_null()) return;

    if (!state->has_value) {
        state->value = input;
        state->count = 1;
        state->has_value = true;
        return;
    }

    switch (func) {
        case AggFunc::kSum:
        case AggFunc::kAvg:
            state->value = state->value + input;
            state->count++;
            break;
        case AggFunc::kMin:
            if (input < state->value) state->value = input;
            state->count++;
            break;
        case AggFunc::kMax:
            if (input > state->value) state->value = input;
            state->count++;
            break;
        case AggFunc::kCount:
            break;
    }
}

static Value finalize_agg(AggFunc func, const AggState& state) {
    if (!state.has_value) {
        if (func == AggFunc::kCount) return Value(static_cast<i64>(0));
        return Value();
    }
    switch (func) {
        case AggFunc::kCount:
            return Value(static_cast<i64>(state.count));
        case AggFunc::kSum:
        case AggFunc::kMin:
        case AggFunc::kMax:
            return state.value;
        case AggFunc::kAvg:
            if (state.count == 0) return Value();
            // Always produce floating-point result to avoid integer truncation.
            bool ok = false;
            double sum = numeric_value_as_double(state.value, &ok);
            return ok ? Value(sum / static_cast<double>(state.count)) : Value();
    }
    return Value();
}

void AggregateExecutor::compute_groups() {
    bool has_group_by = !group_by_.empty();

    if (!has_group_by) {
        bool count_only = !aggregates_.empty() && !having_;
        for (u32 a = 0; a < aggregates_.size(); a++) {
            if (aggregates_[a].func != AggFunc::kCount ||
                aggregates_[a].distinct ||
                aggregates_[a].argument) {
                count_only = false;
                break;
            }
        }
        if (count_only) {
            u64 cnt = 0;
            if (child_->fast_count(&cnt)) {
                Vector<Value> row;
                for (u32 a = 0; a < aggregates_.size(); a++) {
                    row.push_back(Value(static_cast<i64>(cnt)));
                }
                result_groups_.push_back(static_cast<Vector<Value>&&>(row));
                return;
            }
        }

        // No GROUP BY: entire table as one group
        Vector<AggState> states(aggregates_.size());
        while (true) {
            if (executor_cancelled()) return;
            ExecResult r = child_->next();
            if (!r.ok()) break;
            Tuple t = static_cast<Tuple&&>(r.tuple);
            for (u32 a = 0; a < aggregates_.size(); a++) {
                if (aggregates_[a].argument) {
                    Value v = ExpressionEvaluator::evaluate(*aggregates_[a].argument, t);
                    if (!v.is_null()) advance_agg(&states[a], aggregates_[a].func, v,
                                                   aggregates_[a].distinct);
                } else {
                    advance_agg(&states[a], aggregates_[a].func, Value(static_cast<i64>(1)));
                }
            }
        }
        Vector<Value> row;
        for (u32 a = 0; a < aggregates_.size(); a++) {
            row.push_back(finalize_agg(aggregates_[a].func, states[a]));
        }
        // HAVING
        if (having_) {
            Tuple check(output_schema_, row);
            Value cond = ExpressionEvaluator::evaluate(*having_, check);
            if (cond.is_null() || !cond.get_bool()) return;
        }
        result_groups_.push_back(static_cast<Vector<Value>&&>(row));
        return;
    }

    struct Group {
        Vector<Value> key;
        Vector<AggState> states;
    };

    if (work_mem_bytes_ != 0 && compute_groups_sort_spill()) {
        return;
    }

    Vector<Group> groups;
    HashMap<String, u32> group_index;
    u64 memory_used = 0;

    while (true) {
        if (executor_cancelled()) return;
        ExecResult r = child_->next();
        if (!r.ok()) break;
        Tuple tuple = static_cast<Tuple&&>(r.tuple);

        Vector<Value> key;
        for (u32 g = 0; g < group_by_.size(); g++) {
            key.push_back(ExpressionEvaluator::evaluate(*group_by_[g], tuple));
        }

        String group_key = make_values_key(key);
        u32 gidx = 0;
        u32* found = group_index.find(group_key);
        if (found) {
            gidx = *found;
        } else {
            Group ng;
            ng.key = static_cast<Vector<Value>&&>(key);
            ng.states.resize(aggregates_.size());
            gidx = groups.size();
            memory_used += group_key.size() + aggregates_.size() * 96 + 128;
            if (work_mem_bytes_ != 0 && memory_used > work_mem_bytes_) {
                set_executor_error("work_mem exceeded during aggregate");
                return;
            }
            groups.push_back(static_cast<Group&&>(ng));
            group_index.insert(group_key, gidx);
        }

        for (u32 a = 0; a < aggregates_.size(); a++) {
            if (aggregates_[a].argument) {
                Value v = ExpressionEvaluator::evaluate(*aggregates_[a].argument, tuple);
                if (!v.is_null()) advance_agg(&groups[gidx].states[a], aggregates_[a].func, v,
                                               aggregates_[a].distinct);
            } else {
                advance_agg(&groups[gidx].states[a], aggregates_[a].func, Value(static_cast<i64>(1)));
            }
        }
    }

    // Output aggregation result for each group
    for (u32 g = 0; g < groups.size(); g++) {
        Vector<Value> row;
        // Group key columns + aggregate columns
        for (u32 k = 0; k < groups[g].key.size(); k++) {
            row.push_back(groups[g].key[k]);
        }
        for (u32 a = 0; a < aggregates_.size(); a++) {
            row.push_back(finalize_agg(aggregates_[a].func, groups[g].states[a]));
        }
        // HAVING
        if (having_) {
            Tuple check(output_schema_, row);
            Value cond = ExpressionEvaluator::evaluate(*having_, check);
            if (cond.is_null() || !cond.get_bool()) continue;
        }
        result_groups_.push_back(static_cast<Vector<Value>&&>(row));
    }
}

bool AggregateExecutor::compute_groups_sort_spill() {
    struct RunCursor {
        std::FILE* file = nullptr;
        std::string path;
        Tuple tuple;
        String key;
        bool has_tuple = false;
    };

    auto key_for_tuple = [this](const Tuple& tuple) -> String {
        Vector<Value> key_values;
        for (u32 g = 0; g < group_by_.size(); g++) {
            key_values.push_back(ExpressionEvaluator::evaluate(*group_by_[g], tuple));
        }
        return make_values_key(key_values);
    };

    const Schema& input_schema = child_->output_schema();
    std::vector<std::string> run_paths;
    Vector<Tuple> chunk;
    u64 memory_used = 0;

    auto cleanup = [&]() {
        for (const std::string& path : run_paths) unlink(path.c_str());
    };

    auto write_run = [&]() -> bool {
        if (chunk.size() == 0) return true;
        chunk.sort([&](const Tuple& a, const Tuple& b) {
            return key_for_tuple(a) < key_for_tuple(b);
        });
        std::string path_template = temp_dir_;
        if (!path_template.empty() && path_template.back() != '/') path_template += "/";
        path_template += "minidb_agg_XXXXXX";
        std::vector<char> path(path_template.begin(), path_template.end());
        path.push_back('\0');
        int fd = mkstemp(path.data());
        if (fd < 0) {
            set_executor_error("failed to create aggregate spill file");
            return false;
        }
        std::FILE* file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
            unlink(path.data());
            set_executor_error("failed to open aggregate spill file");
            return false;
        }
        for (u32 i = 0; i < chunk.size(); i++) {
            u32 len = chunk[i].serialized_size();
            if (std::fwrite(&len, sizeof(len), 1, file) != 1) {
                std::fclose(file);
                unlink(path.data());
                set_executor_error("failed to write aggregate spill file");
                return false;
            }
            std::vector<byte> bytes(len);
            chunk[i].serialize_to_page(bytes.data());
            bool ok = std::fwrite(bytes.data(), 1, len, file) == len;
            if (!ok) {
                std::fclose(file);
                unlink(path.data());
                set_executor_error("failed to write aggregate spill file");
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
            return true;
        }
        ExecResult r = child_->next();
        if (!r.ok()) break;
        memory_used += r.tuple.serialized_size() + 64;
        chunk.push_back(static_cast<Tuple&&>(r.tuple));
        if (memory_used > work_mem_bytes_ && !write_run()) {
            cleanup();
            return true;
        }
    }

    if (run_paths.empty()) {
        chunk.sort([&](const Tuple& a, const Tuple& b) {
            return key_for_tuple(a) < key_for_tuple(b);
        });
        String current_key;
        Vector<Value> current_values;
        Vector<AggState> states(aggregates_.size());
        bool have_group = false;
        auto flush_group = [&]() {
            if (!have_group) return;
            Vector<Value> row;
            for (u32 i = 0; i < current_values.size(); i++) row.push_back(current_values[i]);
            for (u32 a = 0; a < aggregates_.size(); a++) {
                row.push_back(finalize_agg(aggregates_[a].func, states[a]));
            }
            if (having_) {
                Tuple check(output_schema_, row);
                Value cond = ExpressionEvaluator::evaluate(*having_, check);
                if (cond.is_null() || !cond.get_bool()) return;
            }
            result_groups_.push_back(static_cast<Vector<Value>&&>(row));
        };
        for (u32 i = 0; i < chunk.size(); i++) {
            String k = key_for_tuple(chunk[i]);
            if (!have_group || k != current_key) {
                flush_group();
                current_key = k;
                current_values.clear();
                for (u32 g = 0; g < group_by_.size(); g++) {
                    current_values.push_back(ExpressionEvaluator::evaluate(*group_by_[g], chunk[i]));
                }
                states.clear();
                states.resize(aggregates_.size());
                have_group = true;
            }
            for (u32 a = 0; a < aggregates_.size(); a++) {
                if (aggregates_[a].argument) {
                    Value v = ExpressionEvaluator::evaluate(*aggregates_[a].argument, chunk[i]);
                    if (!v.is_null()) advance_agg(&states[a], aggregates_[a].func, v,
                                                   aggregates_[a].distinct);
                } else {
                    advance_agg(&states[a], aggregates_[a].func, Value(static_cast<i64>(1)));
                }
            }
        }
        flush_group();
        return true;
    }

    if (!write_run()) {
        cleanup();
        return true;
    }

    auto read_tuple = [&](std::FILE* file, Tuple* out) -> bool {
        u32 len = 0;
        if (std::fread(&len, sizeof(len), 1, file) != 1) return false;
        if (len == 0 || len > 64U * 1024U * 1024U) {
            set_executor_error("corrupt aggregate spill file");
            return false;
        }
        std::vector<byte> bytes(len);
        bool ok = std::fread(bytes.data(), 1, len, file) == len;
        if (ok) *out = Tuple::deserialize_from_page(bytes.data(), input_schema, len);
        if (!ok) set_executor_error("failed to read aggregate spill file");
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
            close_cursors();
            cleanup();
            set_executor_error("failed to open aggregate spill file");
            return true;
        }
        c.has_tuple = read_tuple(c.file, &c.tuple);
        if (executor_error()) {
            close_cursors();
            cleanup();
            return true;
        }
        if (c.has_tuple) c.key = key_for_tuple(c.tuple);
        cursors.push_back(static_cast<RunCursor&&>(c));
    }

    String current_key;
    Vector<Value> current_values;
    Vector<AggState> states(aggregates_.size());
    bool have_group = false;
    auto flush_group = [&]() {
        if (!have_group) return;
        Vector<Value> row;
        for (u32 i = 0; i < current_values.size(); i++) row.push_back(current_values[i]);
        for (u32 a = 0; a < aggregates_.size(); a++) {
            row.push_back(finalize_agg(aggregates_[a].func, states[a]));
        }
        if (having_) {
            Tuple check(output_schema_, row);
            Value cond = ExpressionEvaluator::evaluate(*having_, check);
            if (cond.is_null() || !cond.get_bool()) return;
        }
        result_groups_.push_back(static_cast<Vector<Value>&&>(row));
    };

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
        if (!have_group || c.key != current_key) {
            flush_group();
            current_key = c.key;
            current_values.clear();
            for (u32 g = 0; g < group_by_.size(); g++) {
                current_values.push_back(ExpressionEvaluator::evaluate(*group_by_[g], c.tuple));
            }
            states.clear();
            states.resize(aggregates_.size());
            have_group = true;
        }
        for (u32 a = 0; a < aggregates_.size(); a++) {
            if (aggregates_[a].argument) {
                Value v = ExpressionEvaluator::evaluate(*aggregates_[a].argument, c.tuple);
                if (!v.is_null()) advance_agg(&states[a], aggregates_[a].func, v,
                                               aggregates_[a].distinct);
            } else {
                advance_agg(&states[a], aggregates_[a].func, Value(static_cast<i64>(1)));
            }
        }
        c.has_tuple = read_tuple(c.file, &c.tuple);
        if (executor_error()) break;
        if (c.has_tuple) c.key = key_for_tuple(c.tuple);
    }
    flush_group();

    close_cursors();
    cleanup();
    return true;
}

ExecResult AggregateExecutor::next() {
    if (executor_error()) return ExecResult::empty();
    if (cursor_ >= result_groups_.size()) return ExecResult::empty();
    return ExecResult::ok(Tuple(output_schema_, result_groups_[cursor_++]));
}

const Schema& AggregateExecutor::output_schema() const { return output_schema_; }

} // namespace minidb
