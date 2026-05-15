/**
 * @file executor.h
 * @brief Volcano Iterator Model — Executor base class
 */
#pragma once

#include "common/defs.h"
#include "record/tuple.h"
#include "record/schema.h"
#include "index/btree.h"
#include <chrono>

namespace minidb {

inline thread_local const char* g_executor_error_message = nullptr;
inline thread_local bool g_executor_has_deadline = false;
inline thread_local std::chrono::steady_clock::time_point g_executor_deadline;

inline void clear_executor_error() {
    g_executor_error_message = nullptr;
}

inline void set_executor_error(const char* message) {
    g_executor_error_message = message;
}

inline const char* executor_error() {
    return g_executor_error_message;
}

inline void set_executor_deadline_ms(u64 timeout_ms) {
    if (timeout_ms == 0) {
        g_executor_has_deadline = false;
        return;
    }
    g_executor_has_deadline = true;
    g_executor_deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
}

inline bool executor_cancelled() {
    if (!g_executor_has_deadline) return false;
    if (std::chrono::steady_clock::now() < g_executor_deadline) return false;
    set_executor_error("statement timeout");
    return true;
}

struct ExecResult {
    bool has_tuple;
    Tuple tuple;

    bool ok() const { return has_tuple; }
    static ExecResult empty() { return {false, {}}; }
    static ExecResult ok(Tuple t) { return {true, static_cast<Tuple&&>(t)}; }
};

class Executor {
public:
    virtual ~Executor() = default;
    virtual void init() = 0;
    virtual ExecResult next() = 0;
    virtual const Schema& output_schema() const = 0;
    virtual bool fast_count(u64* count) {
        (void)count;
        return false;
    }
    virtual bool last_record_id(RecordId* rid) const {
        (void)rid;
        return false;
    }
};

} // namespace minidb
