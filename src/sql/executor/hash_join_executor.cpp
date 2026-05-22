#include "sql/executor/hash_join_executor.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"
#include <cstring>
#include <vector>
#include <unistd.h>

namespace minidb {

HashJoinExecutor::HashJoinExecutor(
    UniquePtr<Executor> left, UniquePtr<Executor> right,
    UniquePtr<Expression> on_condition, const Schema& output_schema,
    JoinType join_type,
    u64 work_mem_bytes,
    bool build_left,
    const char* temp_dir)
    : left_(static_cast<UniquePtr<Executor>&&>(left)),
      right_(static_cast<UniquePtr<Executor>&&>(right)),
      on_condition_(static_cast<UniquePtr<Expression>&&>(on_condition)),
      output_schema_(output_schema), current_left_(), current_entry_(nullptr),
      left_key_expr_(nullptr), right_key_expr_(nullptr), has_current_left_(false),
      current_left_matched_(false), join_type_(join_type),
      build_left_(join_type == JoinType::kInner && build_left),
      built_(false), work_mem_bytes_(work_mem_bytes), spilled_(false),
      temp_dir_(temp_dir && temp_dir[0] ? temp_dir : "/tmp"),
      right_scan_(nullptr), probe_scan_(nullptr), current_partition_(0),
      grace_ready_(false) {
    std::memset(buckets_, 0, sizeof(buckets_));
}

HashJoinExecutor::~HashJoinExecutor() {
    cleanup_spill();
    for (u32 i = 0; i < all_entries_.size(); i++) {
        delete all_entries_[i];
    }
}

void HashJoinExecutor::init() {
    left_->init();
    right_->init();
    built_ = false;
    current_left_ = Tuple();
    current_entry_ = nullptr;
    left_key_expr_ = nullptr;
    right_key_expr_ = nullptr;
    has_current_left_ = false;
    current_left_matched_ = false;
    spilled_ = false;
    current_partition_ = 0;
    grace_ready_ = false;
    cleanup_spill();

    // Clean up old data
    for (u32 i = 0; i < all_entries_.size(); i++) {
        delete all_entries_[i];
    }
    all_entries_.clear();
    for (u32 i = 0; i < BUCKET_COUNT; i++) buckets_[i] = nullptr;
}

void HashJoinExecutor::cleanup_spill() {
    if (right_scan_) {
        std::fclose(right_scan_);
        right_scan_ = nullptr;
    }
    if (probe_scan_) {
        std::fclose(probe_scan_);
        probe_scan_ = nullptr;
    }
    if (!right_spill_path_.empty()) {
        unlink(right_spill_path_.c_str());
        right_spill_path_.clear();
    }
    for (u32 i = 0; i < GRACE_PARTITIONS; i++) {
        if (!build_partitions_[i].empty()) {
            unlink(build_partitions_[i].c_str());
            build_partitions_[i].clear();
        }
        if (!probe_partitions_[i].empty()) {
            unlink(probe_partitions_[i].c_str());
            probe_partitions_[i].clear();
        }
    }
}

bool HashJoinExecutor::write_spill_tuple(std::FILE* file, const Tuple& tuple) {
    if (!file) return false;
    u32 len = tuple.serialized_size();
    if (std::fwrite(&len, sizeof(len), 1, file) != 1) return false;
    std::vector<byte> bytes(len);
    tuple.serialize_to_page(bytes.data());
    bool ok = std::fwrite(bytes.data(), 1, len, file) == len;
    return ok;
}

bool HashJoinExecutor::read_spill_tuple(std::FILE* file, const Schema& schema, Tuple* out) {
    if (!file || !out) return false;
    u32 len = 0;
    if (std::fread(&len, sizeof(len), 1, file) != 1) return false;
    if (len == 0 || len > 64U * 1024U * 1024U) {
        set_executor_error("corrupt hash join spill file");
        return false;
    }
    std::vector<byte> bytes(len);
    bool ok = std::fread(bytes.data(), 1, len, file) == len;
    if (ok) *out = Tuple::deserialize_from_page(bytes.data(), schema, len);
    if (!ok) set_executor_error("failed to read hash join spill file");
    return ok;
}

u64 HashJoinExecutor::hash_value(const Value& v) {
    u64 h = 0;
    switch (v.type_id()) {
        case TypeId::kInt32:  h = static_cast<u64>(v.get_int32()); break;
        case TypeId::kInt64:  h = static_cast<u64>(v.get_int64()); break;
        case TypeId::kFloat:  { float f = v.get_float(); i32 tmp; std::memcpy(&tmp, &f, 4); h = tmp; break; }
        case TypeId::kDouble: { double d = v.get_double(); i64 tmp; std::memcpy(&tmp, &d, 8); h = tmp; break; }
        case TypeId::kBool:   h = v.get_bool() ? 1 : 0; break;
        case TypeId::kVarchar: {
            const String& s = v.get_string();
            for (u32 i = 0; i < s.size(); i++) {
                h = h * 31 + static_cast<u8>(s[i]);
            }
            break;
        }
        default: break;
    }
    return h % BUCKET_COUNT;
}

Value HashJoinExecutor::eval_key(Expression* expr, const Tuple& tuple) {
    return ExpressionEvaluator::evaluate(*expr, tuple);
}

bool HashJoinExecutor::can_evaluate_on(Expression* expr, const Schema& schema) const {
    if (!expr) return true;
    switch (expr->type) {
        case ExprType::kLiteral:
        case ExprType::kStar:
            return true;
        case ExprType::kColumnRef:
            if (!expr->table_name.empty()) {
                return schema.get_column_index(expr->table_name, expr->column_name) >= 0;
            }
            return schema.get_column_index(expr->column_name) >= 0;
        case ExprType::kBinaryOp:
            return can_evaluate_on(expr->left.get(), schema) &&
                   can_evaluate_on(expr->right.get(), schema);
        case ExprType::kUnaryOp:
            return can_evaluate_on(expr->child.get(), schema);
        case ExprType::kCase:
            for (u32 i = 0; i < expr->when_clauses.size(); i++) {
                if (!can_evaluate_on(expr->when_clauses[i].first.get(), schema) ||
                    !can_evaluate_on(expr->when_clauses[i].second.get(), schema)) {
                    return false;
                }
            }
            return can_evaluate_on(expr->else_expr.get(), schema);
        case ExprType::kCast:
            return can_evaluate_on(expr->child.get(), schema);
        case ExprType::kSubquery:
            return false;
    }
    return false;
}

void HashJoinExecutor::build_hash_table() {
    if (built_) return;
    built_ = true;

    // Extract equi-join keys from ON condition
    // Assumes ON condition is in t1.col = t2.col form
    if (!on_condition_ || on_condition_->type != ExprType::kBinaryOp) return;
    if (on_condition_->op != "=") return;

    Expression* lhs = on_condition_->left.get();
    Expression* rhs = on_condition_->right.get();
    bool lhs_on_left = can_evaluate_on(lhs, left_->output_schema());
    bool lhs_on_right = can_evaluate_on(lhs, right_->output_schema());
    bool rhs_on_left = can_evaluate_on(rhs, left_->output_schema());
    bool rhs_on_right = can_evaluate_on(rhs, right_->output_schema());

    if (lhs_on_left && rhs_on_right) {
        left_key_expr_ = lhs;
        right_key_expr_ = rhs;
    } else if (lhs_on_right && rhs_on_left) {
        left_key_expr_ = rhs;
        right_key_expr_ = lhs;
    } else {
        left_key_expr_ = lhs;
        right_key_expr_ = rhs;
    }

    std::string spill_template = temp_dir_;
    if (!spill_template.empty() && spill_template.back() != '/') spill_template += "/";
    spill_template += "minidb_hash_join_XXXXXX";
    std::vector<char> path(spill_template.begin(), spill_template.end());
    path.push_back('\0');
    int spill_fd = mkstemp(path.data());
    std::FILE* spill_file = nullptr;
    if (spill_fd >= 0) {
        spill_file = fdopen(spill_fd, "wb");
        if (spill_file) right_spill_path_ = path.data();
        else close(spill_fd);
    }

    Executor* build_exec = build_left_ ? left_.get() : right_.get();
    Expression* build_key_expr = build_left_ ? left_key_expr_ : right_key_expr_;

    // Build hash map from the smaller build side. If it crosses work_mem, switch
    // to Grace hash join: partition both sides once and process matching
    // partitions, avoiding the N*M spill fallback.
    u64 memory_used = 0;
    while (true) {
        if (executor_cancelled()) return;
        ExecResult r = build_exec->next();
        if (!r.ok()) break;

        Tuple t = static_cast<Tuple&&>(r.tuple);
        if (spill_file && !write_spill_tuple(spill_file, t)) {
            set_executor_error("failed to write hash join spill file");
            std::fclose(spill_file);
            return;
        }
        memory_used += t.serialized_size() + sizeof(HashJoinEntry) + 32;
        if (work_mem_bytes_ != 0 && memory_used > work_mem_bytes_) {
            if (!spill_file) {
                set_executor_error("failed to create hash join spill file");
                return;
            }
            spilled_ = true;
            for (u32 i = 0; i < all_entries_.size(); i++) delete all_entries_[i];
            all_entries_.clear();
            for (u32 i = 0; i < BUCKET_COUNT; i++) buckets_[i] = nullptr;
            continue;
        }
        if (spilled_) continue;
        Value key = eval_key(build_key_expr, t);
        if (key.is_null()) continue;
        u64 bucket = hash_value(key);

        auto* entry = new HashJoinEntry();
        entry->tuple = static_cast<Tuple&&>(t);
        entry->next = buckets_[bucket];
        buckets_[bucket] = entry;
        all_entries_.push_back(entry);
    }
    if (spill_file) std::fclose(spill_file);
    if (spilled_) {
        if (!prepare_grace_partitions()) return;
    } else {
        cleanup_spill();
    }
}

bool HashJoinExecutor::prepare_grace_partitions() {
    if (grace_ready_) return true;
    if (right_spill_path_.empty()) {
        set_executor_error("missing hash join spill file");
        return false;
    }

    std::FILE* build_out[GRACE_PARTITIONS];
    std::FILE* probe_out[GRACE_PARTITIONS];
    std::memset(build_out, 0, sizeof(build_out));
    std::memset(probe_out, 0, sizeof(probe_out));
    auto close_partitions = [&]() {
        for (u32 j = 0; j < GRACE_PARTITIONS; j++) {
            if (build_out[j]) {
                std::fclose(build_out[j]);
                build_out[j] = nullptr;
            }
            if (probe_out[j]) {
                std::fclose(probe_out[j]);
                probe_out[j] = nullptr;
            }
        }
    };
    auto fail_partitions = [&]() {
        close_partitions();
        cleanup_spill();
    };

    for (u32 i = 0; i < GRACE_PARTITIONS; i++) {
        std::string btmpl = temp_dir_;
        if (!btmpl.empty() && btmpl.back() != '/') btmpl += "/";
        btmpl += "minidb_hj_build_XXXXXX";
        std::vector<char> bpath(btmpl.begin(), btmpl.end());
        bpath.push_back('\0');
        int bfd = mkstemp(bpath.data());
        if (bfd < 0) {
            fail_partitions();
            set_executor_error("failed to create hash join build partition");
            return false;
        }
        build_out[i] = fdopen(bfd, "wb");
        if (!build_out[i]) {
            close(bfd);
            unlink(bpath.data());
            fail_partitions();
            set_executor_error("failed to open hash join build partition");
            return false;
        }
        build_partitions_[i] = bpath.data();

        std::string ptmpl = temp_dir_;
        if (!ptmpl.empty() && ptmpl.back() != '/') ptmpl += "/";
        ptmpl += "minidb_hj_probe_XXXXXX";
        std::vector<char> ppath(ptmpl.begin(), ptmpl.end());
        ppath.push_back('\0');
        int pfd = mkstemp(ppath.data());
        if (pfd < 0) {
            fail_partitions();
            set_executor_error("failed to create hash join probe partition");
            return false;
        }
        probe_out[i] = fdopen(pfd, "wb");
        if (!probe_out[i]) {
            close(pfd);
            unlink(ppath.data());
            fail_partitions();
            set_executor_error("failed to open hash join probe partition");
            return false;
        }
        probe_partitions_[i] = ppath.data();
    }

    std::FILE* build_in = std::fopen(right_spill_path_.c_str(), "rb");
    if (!build_in) {
        fail_partitions();
        set_executor_error("failed to read hash join spill file");
        return false;
    }

    const Schema& build_schema = build_left_ ? left_->output_schema() : right_->output_schema();
    Expression* build_key_expr = build_left_ ? left_key_expr_ : right_key_expr_;
    Expression* probe_key_expr = build_left_ ? right_key_expr_ : left_key_expr_;

    Tuple tuple;
    while (read_spill_tuple(build_in, build_schema, &tuple)) {
        Value key = eval_key(build_key_expr, tuple);
        u32 part = key.is_null() ? 0 : static_cast<u32>(hash_value(key) % GRACE_PARTITIONS);
        if (!write_spill_tuple(build_out[part], tuple)) {
            std::fclose(build_in);
            fail_partitions();
            set_executor_error("failed to write hash join build partition");
            return false;
        }
    }
    std::fclose(build_in);
    if (executor_error()) {
        fail_partitions();
        return false;
    }

    Executor* probe_exec = build_left_ ? right_.get() : left_.get();
    while (true) {
        if (executor_cancelled()) {
            fail_partitions();
            return false;
        }
        ExecResult r = probe_exec->next();
        if (!r.ok()) break;
        Tuple pt = static_cast<Tuple&&>(r.tuple);
        Value key = eval_key(probe_key_expr, pt);
        u32 part = key.is_null() ? 0 : static_cast<u32>(hash_value(key) % GRACE_PARTITIONS);
        if (!write_spill_tuple(probe_out[part], pt)) {
            fail_partitions();
            set_executor_error("failed to write hash join probe partition");
            return false;
        }
    }

    close_partitions();
    if (!right_spill_path_.empty()) {
        unlink(right_spill_path_.c_str());
        right_spill_path_.clear();
    }
    grace_ready_ = true;
    current_partition_ = 0;
    return true;
}

bool HashJoinExecutor::load_next_partition() {
    if (probe_scan_) {
        std::fclose(probe_scan_);
        probe_scan_ = nullptr;
    }
    for (u32 i = 0; i < all_entries_.size(); i++) delete all_entries_[i];
    all_entries_.clear();
    for (u32 i = 0; i < BUCKET_COUNT; i++) buckets_[i] = nullptr;
    has_current_left_ = false;
    current_entry_ = nullptr;

    const Schema& build_schema = build_left_ ? left_->output_schema() : right_->output_schema();
    Expression* build_key_expr = build_left_ ? left_key_expr_ : right_key_expr_;

    while (current_partition_ < GRACE_PARTITIONS) {
        std::FILE* build_in = std::fopen(build_partitions_[current_partition_].c_str(), "rb");
        if (build_in) {
            Tuple bt;
            while (read_spill_tuple(build_in, build_schema, &bt)) {
                Value key = eval_key(build_key_expr, bt);
                if (!key.is_null()) {
                    auto* entry = new HashJoinEntry();
                    entry->tuple = static_cast<Tuple&&>(bt);
                    u64 bucket = hash_value(key);
                    entry->next = buckets_[bucket];
                    buckets_[bucket] = entry;
                    all_entries_.push_back(entry);
                }
            }
            std::fclose(build_in);
            if (executor_error()) return false;
        }

        probe_scan_ = std::fopen(probe_partitions_[current_partition_].c_str(), "rb");
        current_partition_++;
        if (probe_scan_) return true;
    }
    return false;
}

ExecResult HashJoinExecutor::make_join_tuple(const Tuple& probe, const Tuple& build) {
    Vector<Value> values;
    if (build_left_) {
        for (u32 i = 0; i < build.column_count(); i++) values.push_back(build.get_value(i));
        for (u32 i = 0; i < probe.column_count(); i++) values.push_back(probe.get_value(i));
    } else {
        for (u32 i = 0; i < probe.column_count(); i++) values.push_back(probe.get_value(i));
        for (u32 i = 0; i < build.column_count(); i++) values.push_back(build.get_value(i));
    }
    return ExecResult::ok(Tuple(output_schema_, values));
}

ExecResult HashJoinExecutor::next() {
    build_hash_table();
    if (executor_error()) return ExecResult::empty();
    if (!left_key_expr_ || !right_key_expr_) return ExecResult::empty();
    if (spilled_) return next_spilled();

    while (true) {
        if (executor_cancelled()) return ExecResult::empty();
        while (has_current_left_ && current_entry_) {
            HashJoinEntry* entry = current_entry_;
            current_entry_ = current_entry_->next;

            Expression* probe_key_expr = build_left_ ? right_key_expr_ : left_key_expr_;
            Expression* build_key_expr = build_left_ ? left_key_expr_ : right_key_expr_;
            Value probe_key = eval_key(probe_key_expr, current_left_);
            Value build_key = eval_key(build_key_expr, entry->tuple);
            if (!probe_key.is_null() && !build_key.is_null() && probe_key == build_key) {
                current_left_matched_ = true;
                return make_join_tuple(current_left_, entry->tuple);
            }
        }
        if (has_current_left_ && join_type_ == JoinType::kLeft && !current_left_matched_) {
            Vector<Value> values;
            for (u32 i = 0; i < current_left_.column_count(); i++) {
                values.push_back(current_left_.get_value(i));
            }
            u32 right_cols = output_schema_.column_count() > current_left_.column_count()
                             ? output_schema_.column_count() - current_left_.column_count() : 0;
            for (u32 i = 0; i < right_cols; i++) {
                values.push_back(Value());
            }
            has_current_left_ = false;
            return ExecResult::ok(Tuple(output_schema_, values));
        }

        Executor* probe_exec = build_left_ ? right_.get() : left_.get();
        ExecResult r = probe_exec->next();
        if (!r.ok()) return ExecResult::empty();
        current_left_ = static_cast<Tuple&&>(r.tuple);
        has_current_left_ = true;
        current_left_matched_ = false;

        Expression* probe_key_expr = build_left_ ? right_key_expr_ : left_key_expr_;
        Value key = eval_key(probe_key_expr, current_left_);
        if (key.is_null()) {
            current_entry_ = nullptr;
            continue;
        }
        current_entry_ = buckets_[hash_value(key)];
    }
}

ExecResult HashJoinExecutor::next_spilled() {
    while (true) {
        if (executor_cancelled()) return ExecResult::empty();
        if (!probe_scan_) {
            if (!load_next_partition()) return ExecResult::empty();
        }
        if (!has_current_left_) {
            const Schema& probe_schema = build_left_ ? right_->output_schema() : left_->output_schema();
            Tuple probe_tuple;
            if (!read_spill_tuple(probe_scan_, probe_schema, &probe_tuple)) {
                if (executor_error()) return ExecResult::empty();
                if (probe_scan_) {
                    std::fclose(probe_scan_);
                    probe_scan_ = nullptr;
                }
                continue;
            }
            current_left_ = static_cast<Tuple&&>(probe_tuple);
            has_current_left_ = true;
            current_left_matched_ = false;
            Expression* probe_key_expr = build_left_ ? right_key_expr_ : left_key_expr_;
            Value key = eval_key(probe_key_expr, current_left_);
            current_entry_ = key.is_null() ? nullptr : buckets_[hash_value(key)];
        }

        while (current_entry_) {
            HashJoinEntry* entry = current_entry_;
            current_entry_ = current_entry_->next;
            Expression* probe_key_expr = build_left_ ? right_key_expr_ : left_key_expr_;
            Expression* build_key_expr = build_left_ ? left_key_expr_ : right_key_expr_;
            Value probe_key = eval_key(probe_key_expr, current_left_);
            Value build_key = eval_key(build_key_expr, entry->tuple);
            if (!probe_key.is_null() && !build_key.is_null() && probe_key == build_key) {
                current_left_matched_ = true;
                return make_join_tuple(current_left_, entry->tuple);
            }
        }
        if (executor_error()) return ExecResult::empty();
        if (join_type_ == JoinType::kLeft && !current_left_matched_) {
            Vector<Value> values;
            for (u32 i = 0; i < current_left_.column_count(); i++) {
                values.push_back(current_left_.get_value(i));
            }
            u32 right_cols = output_schema_.column_count() > current_left_.column_count()
                             ? output_schema_.column_count() - current_left_.column_count() : 0;
            for (u32 i = 0; i < right_cols; i++) values.push_back(Value());
            has_current_left_ = false;
            return ExecResult::ok(Tuple(output_schema_, values));
        }
        has_current_left_ = false;
    }
}

const Schema& HashJoinExecutor::output_schema() const { return output_schema_; }

bool HashJoinExecutor::fast_count(u64* count) {
    if (!count) return false;
    build_hash_table();
    if (executor_error() || !left_key_expr_ || !right_key_expr_) return false;

    u64 total = 0;
    if (!spilled_) {
        Executor* probe_exec = build_left_ ? right_.get() : left_.get();
        Expression* probe_key_expr = build_left_ ? right_key_expr_ : left_key_expr_;
        Expression* build_key_expr = build_left_ ? left_key_expr_ : right_key_expr_;
        while (true) {
            if (executor_cancelled()) return false;
            ExecResult r = probe_exec->next();
            if (!r.ok()) break;
            Value probe_key = eval_key(probe_key_expr, r.tuple);
            bool matched = false;
            if (!probe_key.is_null()) {
                HashJoinEntry* entry = buckets_[hash_value(probe_key)];
                while (entry) {
                    Value build_key = eval_key(build_key_expr, entry->tuple);
                    if (!build_key.is_null() && probe_key == build_key) {
                        total++;
                        matched = true;
                    }
                    entry = entry->next;
                }
            }
            if (join_type_ == JoinType::kLeft && !matched) total++;
        }
        *count = total;
        return true;
    }

    while (true) {
        if (!probe_scan_) {
            if (!load_next_partition()) break;
        }
        const Schema& probe_schema = build_left_ ? right_->output_schema() : left_->output_schema();
        Tuple probe_tuple;
        if (!read_spill_tuple(probe_scan_, probe_schema, &probe_tuple)) {
            if (executor_error()) return false;
            if (probe_scan_) {
                std::fclose(probe_scan_);
                probe_scan_ = nullptr;
            }
            continue;
        }
        Expression* probe_key_expr = build_left_ ? right_key_expr_ : left_key_expr_;
        Expression* build_key_expr = build_left_ ? left_key_expr_ : right_key_expr_;
        Value probe_key = eval_key(probe_key_expr, probe_tuple);
        bool matched = false;
        if (!probe_key.is_null()) {
            HashJoinEntry* entry = buckets_[hash_value(probe_key)];
            while (entry) {
                Value build_key = eval_key(build_key_expr, entry->tuple);
                if (!build_key.is_null() && probe_key == build_key) {
                    total++;
                    matched = true;
                }
                entry = entry->next;
            }
        }
        if (join_type_ == JoinType::kLeft && !matched) total++;
    }
    *count = total;
    return !executor_error();
}

} // namespace minidb
