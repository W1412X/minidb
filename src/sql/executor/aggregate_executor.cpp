#include "sql/executor/aggregate_executor.h"
#include "common/tuple_key.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"
#include <algorithm>
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

// Accumulate integer SUM/AVG in 64-bit. Without this an int32 column whose
// running total exceeds INT32_MAX produces a NULL (Value::operator+ returns
// NULL on int32 overflow), so SUM/AVG silently collapse to NULL even though
// every individual value is in range — and the planner already declares the
// output type as int64.
static Value widen_int_for_sum(const Value& v) {
    if (v.type_id() == TypeId::kInt32) return Value(static_cast<i64>(v.get_int32()));
    if (v.type_id() == TypeId::kBool)  return Value(static_cast<i64>(v.get_bool() ? 1 : 0));
    return v;
}

static bool having_passes(const Value& cond) {
    bool pass = false;
    if (!ExpressionEvaluator::predicate_truth(cond, &pass)) {
        set_executor_error("HAVING expression must be BOOL");
        return false;
    }
    return pass;
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
        state->value = (func == AggFunc::kSum || func == AggFunc::kAvg)
                           ? widen_int_for_sum(input)
                           : input;
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
        Vector<Value> fast_row;
        if (child_->fast_plain_aggregate(aggregates_, &fast_row)) {
            result_groups_.push_back(static_cast<Vector<Value>&&>(fast_row));
            return;
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
            if (!having_passes(cond)) return;
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
            if (!having_passes(cond)) continue;
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
    // Each buffered tuple carries its precomputed group key so neither the run
    // sort nor the in-memory sort recomputes it on every comparison. Before,
    // the sort comparator called key_for_tuple() (evaluate group exprs + build
    // a String) twice per comparison — O(N log N) String allocations per run,
    // which was both the dominant CPU cost and the malloc high-water for
    // high-cardinality GROUP BY.
    struct Keyed { String key; Tuple tuple; };
    Vector<Keyed> chunk;
    u64 memory_used = 0;

    auto cleanup = [&]() {
        for (const std::string& path : run_paths) unlink(path.c_str());
    };

    auto write_run = [&]() -> bool {
        if (chunk.size() == 0) return true;
        chunk.sort([](const Keyed& a, const Keyed& b) { return a.key < b.key; });
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
            u32 len = chunk[i].tuple.serialized_size();
            if (std::fwrite(&len, sizeof(len), 1, file) != 1) {
                std::fclose(file);
                unlink(path.data());
                set_executor_error("failed to write aggregate spill file");
                return false;
            }
            std::vector<byte> bytes(len);
            chunk[i].tuple.serialize_to_page(bytes.data());
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
        Keyed kt;
        kt.key = key_for_tuple(r.tuple);
        memory_used += r.tuple.serialized_size() + kt.key.size() + 64;
        kt.tuple = static_cast<Tuple&&>(r.tuple);
        chunk.push_back(static_cast<Keyed&&>(kt));
        if (memory_used > work_mem_bytes_ && !write_run()) {
            cleanup();
            return true;
        }
    }

    if (run_paths.empty()) {
        chunk.sort([](const Keyed& a, const Keyed& b) { return a.key < b.key; });
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
                if (!having_passes(cond)) return;
            }
            result_groups_.push_back(static_cast<Vector<Value>&&>(row));
        };
        for (u32 i = 0; i < chunk.size(); i++) {
            const Tuple& t = chunk[i].tuple;
            if (!have_group || chunk[i].key != current_key) {
                flush_group();
                current_key = chunk[i].key;
                current_values.clear();
                for (u32 g = 0; g < group_by_.size(); g++) {
                    current_values.push_back(ExpressionEvaluator::evaluate(*group_by_[g], t));
                }
                states.clear();
                states.resize(aggregates_.size());
                have_group = true;
            }
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

    std::vector<u32> merge_heap;
    merge_heap.reserve(cursors.size());
    for (u32 i = 0; i < cursors.size(); i++) {
        if (cursors[i].has_tuple) merge_heap.push_back(i);
    }
    auto worse_cursor = [&cursors](u32 a, u32 b) {
        return cursors[a].key > cursors[b].key;
    };
    std::make_heap(merge_heap.begin(), merge_heap.end(), worse_cursor);

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
            if (!having_passes(cond)) return;
        }
        result_groups_.push_back(static_cast<Vector<Value>&&>(row));
    };

    while (!merge_heap.empty()) {
        if (executor_cancelled()) break;
        std::pop_heap(merge_heap.begin(), merge_heap.end(), worse_cursor);
        u32 best = merge_heap.back();
        merge_heap.pop_back();
        RunCursor& c = cursors[best];
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
        if (c.has_tuple) {
            c.key = key_for_tuple(c.tuple);
            merge_heap.push_back(best);
            std::push_heap(merge_heap.begin(), merge_heap.end(), worse_cursor);
        }
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
