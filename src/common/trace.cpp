#include "common/trace.h"
#include <cstdio>

namespace minidb {

thread_local TraceContext* g_trace_context = nullptr;

TraceContext::TraceContext() : TraceContext(TraceOptions()) {}

TraceContext::TraceContext(const TraceOptions& options)
    : start_(std::chrono::steady_clock::now()), options_(options) {
    if (options_.level == TraceLevel::kDebug && options_.max_events == 0) {
        options_.max_events = 10000;
    }
    if (!options_.events_path.empty()) {
        event_file_ = std::fopen(options_.events_path.c_str(), "w");
        options_.include_events = false;
        options_.channels |= kTraceEvent;
    }
}

TraceContext::~TraceContext() {
    if (event_file_) {
        std::fclose(event_file_);
        event_file_ = nullptr;
    }
}

u64 TraceContext::now_us() const {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_).count());
}

TraceOperatorStats& TraceContext::operator_stats(u32 node_id, const char* executor_type) {
    for (auto& s : operators_) {
        if (s.node_id == node_id) {
            if (executor_type && s.executor_type.empty()) s.executor_type = executor_type;
            return s;
        }
    }
    TraceOperatorStats s;
    s.node_id = node_id;
    if (executor_type) s.executor_type = executor_type;
    operators_.push_back(s);
    return operators_.back();
}

TraceOperatorStats& TraceContext::current_operator() {
    return operator_stats(current_node_id_);
}

void TraceContext::record_event(const char* phase, const char* name, const String& detail) {
    maybe_event(kTraceEvent, phase, name, detail);
}

bool TraceContext::should_record_event(TraceChannel channel) const {
    if (!channel_enabled(kTraceEvent) && !event_file_ && !options_.include_events) return false;
    if (!channel_enabled(channel)) return false;
    return level_at_least(TraceLevel::kDebug) || event_file_ || options_.include_events;
}

void TraceContext::write_event_ndjson(const TraceEvent& event) {
    if (!event_file_) return;
    std::fprintf(event_file_,
                 "{\"ts_us\":%llu,\"node_id\":%u,\"channel\":\"%s\",\"event\":\"%s\",\"detail\":",
                 static_cast<unsigned long long>(event.ts_us), event.node_id,
                 event.phase.c_str(), event.name.c_str());
    if (event.detail.empty()) {
        std::fprintf(event_file_, "null");
    } else {
        std::fprintf(event_file_, "%s", event.detail.c_str());
    }
    std::fprintf(event_file_, "}\n");
}

void TraceContext::maybe_event(TraceChannel channel, const char* phase, const char* name, const String& detail) {
    if (!should_record_event(channel)) return;
    TraceEvent e;
    e.ts_us = now_us();
    e.node_id = current_node_id_;
    e.phase = phase ? phase : "";
    e.name = name ? name : "";
    e.detail = detail;
    options_.events_recorded++;
    write_event_ndjson(e);
    if (!options_.include_events) return;
    if (options_.max_events != 0 && events_.size() >= options_.max_events) {
        options_.events_dropped++;
        return;
    }
    events_.push_back(e);
}

void TraceContext::record_buffer_fetch(PageId page_id, bool hit, bool sequential) {
    if (!channel_enabled(kTraceStorage)) return;
    buffer_fetches_++;
    if (hit) buffer_hits_++; else buffer_misses_++;
    TraceOperatorStats& op = current_operator();
    op.buffer_fetches++;
    if (hit) op.buffer_hits++; else op.buffer_misses++;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"page_id\":%llu,\"hit\":%s,\"sequential\":%s}",
                  static_cast<unsigned long long>(page_id), hit ? "true" : "false",
                  sequential ? "true" : "false");
    maybe_event(kTraceStorage, "storage", "buffer_fetch", String(buf));
}

void TraceContext::record_buffer_new_page(PageId page_id) {
    if (!channel_enabled(kTraceStorage)) return;
    buffer_new_pages_++;
    current_operator().buffer_new_pages++;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"page_id\":%llu}",
                  static_cast<unsigned long long>(page_id));
    maybe_event(kTraceStorage, "storage", "buffer_new_page", String(buf));
}

void TraceContext::record_buffer_dirty(PageId page_id) {
    if (!channel_enabled(kTraceStorage)) return;
    buffer_dirty_marks_++;
    current_operator().buffer_dirty_marks++;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"page_id\":%llu}",
                  static_cast<unsigned long long>(page_id));
    maybe_event(kTraceStorage, "storage", "buffer_mark_dirty", String(buf));
}

void TraceContext::record_buffer_flush(PageId page_id) {
    if (!channel_enabled(kTraceStorage)) return;
    buffer_flush_pages_++;
    current_operator().buffer_flush_pages++;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"page_id\":%llu}",
                  static_cast<unsigned long long>(page_id));
    maybe_event(kTraceStorage, "storage", "buffer_flush_page", String(buf));
}

void TraceContext::record_buffer_eviction(PageId old_page_id, PageId new_page_id, bool dirty) {
    if (!channel_enabled(kTraceStorage)) return;
    buffer_evictions_++;
    current_operator().buffer_evictions++;
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "{\"old_page_id\":%llu,\"new_page_id\":%llu,\"dirty\":%s}",
                  static_cast<unsigned long long>(old_page_id),
                  static_cast<unsigned long long>(new_page_id),
                  dirty ? "true" : "false");
    maybe_event(kTraceStorage, "storage", "buffer_evict", String(buf));
}

void TraceContext::record_heap_tuple(bool visible) {
    if (!channel_enabled(kTraceMvcc) && !level_at_least(TraceLevel::kVerbose)) return;
    TraceOperatorStats& op = current_operator();
    op.heap_tuples_seen++;
    if (visible) op.heap_tuples_visible++; else op.heap_tuples_invisible++;
}

void TraceContext::record_heap_filter() {
    if (!channel_enabled(kTraceExecutor) && !channel_enabled(kTraceMvcc)) return;
    current_operator().heap_tuples_filtered++;
}

void TraceContext::record_version_chain_step() {
    if (!channel_enabled(kTraceMvcc)) return;
    current_operator().version_chain_steps++;
    maybe_event(kTraceMvcc, "mvcc", "version_chain_step", String(""));
}

void TraceContext::record_index_batch(u64 keys) {
    if (!channel_enabled(kTraceIndex)) return;
    TraceOperatorStats& op = current_operator();
    op.index_keys_examined += keys;
    op.index_rids_returned += keys;
}

void TraceContext::record_index_recheck(bool visible) {
    if (!channel_enabled(kTraceIndex)) return;
    TraceOperatorStats& op = current_operator();
    op.index_heap_rechecks++;
    op.heap_tuples_seen++;
    if (visible) op.heap_tuples_visible++; else op.heap_tuples_invisible++;
}

void TraceContext::record_wal(const char* type, u64 lsn, u64 txn_id, u32 bytes) {
    if (!channel_enabled(kTraceWal)) return;
    wal_records_++;
    wal_bytes_ += bytes;
    TraceOperatorStats& op = current_operator();
    op.wal_records++;
    op.wal_bytes += bytes;
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "{\"type\":\"%s\",\"lsn\":%llu,\"txn_id\":%llu,\"bytes\":%u}",
                  type ? type : "unknown",
                  static_cast<unsigned long long>(lsn),
                  static_cast<unsigned long long>(txn_id), bytes);
    maybe_event(kTraceWal, "wal", "write_record", String(buf));
}

void TraceContext::record_lock(const char* kind, u64 txn_id, u32 table_id,
                               bool waited, u64 wait_us, bool ok) {
    if (!channel_enabled(kTraceLock)) return;
    lock_acquires_++;
    if (waited) {
        lock_waits_++;
        lock_wait_us_ += wait_us;
    }
    TraceOperatorStats& op = current_operator();
    op.lock_acquires++;
    if (waited) {
        op.lock_waits++;
        op.lock_wait_us += wait_us;
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "{\"kind\":\"%s\",\"txn_id\":%llu,\"table_id\":%u,\"wait_us\":%llu,\"ok\":%s}",
                  kind ? kind : "unknown",
                  static_cast<unsigned long long>(txn_id), table_id,
                  static_cast<unsigned long long>(wait_us), ok ? "true" : "false");
    maybe_event(kTraceLock, "lock", waited ? "wait" : "acquire", String(buf));
}

} // namespace minidb
