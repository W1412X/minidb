/**
 * @file trace.h
 * @brief Optional structured execution tracing for EXPLAIN TRACE.
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include "storage/page.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace minidb {

enum class TraceLevel : u8 {
    kSummary = 0,
    kNormal = 1,
    kVerbose = 2,
    kDebug = 3,
};

enum TraceChannel : u32 {
    kTracePipeline = 1u << 0,
    kTracePlan     = 1u << 1,
    kTraceExecutor = 1u << 2,
    kTraceStorage  = 1u << 3,
    kTraceMvcc     = 1u << 4,
    kTraceIndex    = 1u << 5,
    kTraceWal      = 1u << 6,
    kTraceLock     = 1u << 7,
    kTraceMemory   = 1u << 8,
    kTraceEvent    = 1u << 9,
};

static constexpr u32 kTraceDefaultChannels =
    kTracePipeline | kTracePlan | kTraceExecutor | kTraceStorage |
    kTraceIndex | kTraceWal | kTraceLock | kTraceMemory;

static constexpr u32 kTraceAllChannels =
    kTracePipeline | kTracePlan | kTraceExecutor | kTraceStorage |
    kTraceMvcc | kTraceIndex | kTraceWal | kTraceLock |
    kTraceMemory | kTraceEvent;

struct TraceOptions {
    TraceLevel level = TraceLevel::kNormal;
    u32 channels = kTraceDefaultChannels;
    bool include_events = false;
    String events_path;
    u64 max_events = 0;
    u64 events_recorded = 0;
    u64 events_dropped = 0;

    bool channel_enabled(TraceChannel channel) const {
        return (channels & static_cast<u32>(channel)) != 0;
    }
};

struct TraceOperatorStats {
    u32 node_id = 0;
    String executor_type;
    u64 init_calls = 0;
    u64 next_calls = 0;
    u64 output_rows = 0;
    u64 init_us = 0;
    u64 next_us = 0;

    u64 buffer_fetches = 0;
    u64 buffer_hits = 0;
    u64 buffer_misses = 0;
    u64 buffer_new_pages = 0;
    u64 buffer_dirty_marks = 0;
    u64 buffer_flush_pages = 0;
    u64 buffer_evictions = 0;

    u64 heap_tuples_seen = 0;
    u64 heap_tuples_visible = 0;
    u64 heap_tuples_invisible = 0;
    u64 heap_tuples_filtered = 0;
    u64 version_chain_steps = 0;

    u64 index_keys_examined = 0;
    u64 index_rids_returned = 0;
    u64 index_heap_rechecks = 0;

    u64 wal_records = 0;
    u64 wal_bytes = 0;
    u64 lock_acquires = 0;
    u64 lock_waits = 0;
    u64 lock_wait_us = 0;
};

struct TraceEvent {
    u64 ts_us = 0;
    u32 node_id = 0;
    String phase;
    String name;
    String detail;
};

class TraceContext {
public:
    TraceContext();
    explicit TraceContext(const TraceOptions& options);
    ~TraceContext();

    u64 now_us() const;
    const TraceOptions& options() const { return options_; }
    TraceOptions& mutable_options() { return options_; }
    bool channel_enabled(TraceChannel channel) const { return options_.channel_enabled(channel); }
    bool level_at_least(TraceLevel level) const {
        return static_cast<u8>(options_.level) >= static_cast<u8>(level);
    }
    u32 current_node_id() const { return current_node_id_; }
    void set_current_node_id(u32 node_id) { current_node_id_ = node_id; }

    TraceOperatorStats& operator_stats(u32 node_id, const char* executor_type = nullptr);
    const std::vector<TraceOperatorStats>& operators() const { return operators_; }
    const std::vector<TraceEvent>& events() const { return events_; }

    void record_event(const char* phase, const char* name, const String& detail = String(""));
    void record_buffer_fetch(PageId page_id, bool hit, bool sequential);
    void record_buffer_new_page(PageId page_id);
    void record_buffer_dirty(PageId page_id);
    void record_buffer_flush(PageId page_id);
    void record_buffer_eviction(PageId old_page_id, PageId new_page_id, bool dirty);
    void record_heap_tuple(bool visible);
    void record_heap_filter();
    void record_version_chain_step();
    void record_index_batch(u64 keys);
    void record_index_recheck(bool visible);
    void record_wal(const char* type, u64 lsn, u64 txn_id, u32 bytes);
    void record_lock(const char* kind, u64 txn_id, u32 table_id, bool waited, u64 wait_us, bool ok);

    u64 buffer_fetches() const { return buffer_fetches_; }
    u64 buffer_hits() const { return buffer_hits_; }
    u64 buffer_misses() const { return buffer_misses_; }
    u64 buffer_new_pages() const { return buffer_new_pages_; }
    u64 buffer_dirty_marks() const { return buffer_dirty_marks_; }
    u64 buffer_flush_pages() const { return buffer_flush_pages_; }
    u64 buffer_evictions() const { return buffer_evictions_; }
    u64 wal_records() const { return wal_records_; }
    u64 wal_bytes() const { return wal_bytes_; }
    u64 lock_acquires() const { return lock_acquires_; }
    u64 lock_waits() const { return lock_waits_; }
    u64 lock_wait_us() const { return lock_wait_us_; }

private:
    bool should_record_event(TraceChannel channel) const;
    void write_event_ndjson(const TraceEvent& event);
    TraceOperatorStats& current_operator();
    void maybe_event(TraceChannel channel, const char* phase, const char* name, const String& detail);

    std::chrono::steady_clock::time_point start_;
    TraceOptions options_;
    u32 current_node_id_ = 0;
    std::vector<TraceOperatorStats> operators_;
    std::vector<TraceEvent> events_;
    std::FILE* event_file_ = nullptr;

    u64 buffer_fetches_ = 0;
    u64 buffer_hits_ = 0;
    u64 buffer_misses_ = 0;
    u64 buffer_new_pages_ = 0;
    u64 buffer_dirty_marks_ = 0;
    u64 buffer_flush_pages_ = 0;
    u64 buffer_evictions_ = 0;
    u64 wal_records_ = 0;
    u64 wal_bytes_ = 0;
    u64 lock_acquires_ = 0;
    u64 lock_waits_ = 0;
    u64 lock_wait_us_ = 0;
};

class TraceNodeScope {
public:
    TraceNodeScope(TraceContext* ctx, u32 node_id)
        : ctx_(ctx), prev_(ctx ? ctx->current_node_id() : 0) {
        if (ctx_) ctx_->set_current_node_id(node_id);
    }
    ~TraceNodeScope() {
        if (ctx_) ctx_->set_current_node_id(prev_);
    }

private:
    TraceContext* ctx_;
    u32 prev_;
};

extern thread_local TraceContext* g_trace_context;

inline TraceContext* current_trace() { return g_trace_context; }

} // namespace minidb
