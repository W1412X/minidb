#include "sql/executor/union_executor.h"
#include "common/tuple_key.h"
#include "container/hash_map.h"

namespace minidb {

UnionExecutor::UnionExecutor(UniquePtr<Executor> left, UniquePtr<Executor> right,
                             bool all, const Schema& output_schema)
    : left_(static_cast<UniquePtr<Executor>&&>(left)),
      right_(static_cast<UniquePtr<Executor>&&>(right)),
      all_(all), output_schema_(output_schema), left_done_(false), cursor_(0) {}

void UnionExecutor::init() {
    left_->init();
    right_->init();
    left_done_ = false;
}

ExecResult UnionExecutor::next() {
    if (all_) {
        // UNION ALL: direct concatenation
        if (!left_done_) {
            ExecResult r = left_->next();
            if (r.ok()) return r;
            left_done_ = true;
        }
        return right_->next();
    }

    // UNION (deduplicate): materialize all rows, output non-duplicate ones
    // Materialize on first call
    if (!left_done_) {
        left_done_ = true;
        buffer_.clear();
        HashMap<String, bool> seen;
        while (true) {
            ExecResult r = left_->next();
            if (!r.ok()) break;
            String key = make_tuple_key(r.tuple);
            if (!seen.find(key)) {
                seen.insert(key, true);
                buffer_.push_back(static_cast<Tuple&&>(r.tuple));
            }
        }
        while (true) {
            ExecResult r = right_->next();
            if (!r.ok()) break;
            Tuple t = static_cast<Tuple&&>(r.tuple);
            String key = make_tuple_key(t);
            if (!seen.find(key)) {
                seen.insert(key, true);
                buffer_.push_back(static_cast<Tuple&&>(t));
            }
        }
        cursor_ = 0;
    }

    if (cursor_ >= buffer_.size()) return ExecResult::empty();
    return ExecResult::ok(static_cast<Tuple&&>(buffer_[cursor_++]));
}

bool UnionExecutor::tuples_equal(const Tuple& a, const Tuple& b) const {
    if (a.column_count() != b.column_count()) return false;
    for (u32 i = 0; i < a.column_count(); i++) {
        if (a.get_value(i) != b.get_value(i)) return false;
    }
    return true;
}

const Schema& UnionExecutor::output_schema() const { return output_schema_; }

} // namespace minidb
