#include "sql/executor/limit_executor.h"

namespace minidb {

LimitExecutor::LimitExecutor(UniquePtr<Executor> child, i32 limit, i32 offset)
    : child_(static_cast<UniquePtr<Executor>&&>(child)),
      limit_(limit), offset_(offset),
      rows_returned_(0), rows_skipped_(0), done_(false) {}

void LimitExecutor::init() {
    child_->init();
    rows_returned_ = 0;
    rows_skipped_ = 0;
    done_ = false;
}

ExecResult LimitExecutor::next() {
    if (done_) return ExecResult::empty();

    // skip OFFSET rows
    while (rows_skipped_ < offset_) {
        ExecResult r = child_->next();
        if (!r.ok()) { done_ = true; return ExecResult::empty(); }
        rows_skipped_++;
    }

    // return up to LIMIT rows
    if (limit_ >= 0 && rows_returned_ >= limit_) {
        done_ = true;
        return ExecResult::empty();
    }

    ExecResult r = child_->next();
    if (!r.ok()) { done_ = true; return ExecResult::empty(); }
    rows_returned_++;
    if (limit_ >= 0 && rows_returned_ >= limit_) done_ = true;
    return r;
}

const Schema& LimitExecutor::output_schema() const { return child_->output_schema(); }

} // namespace minidb
