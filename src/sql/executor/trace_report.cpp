#include "sql/executor/trace_report.h"
#include <cstdio>

namespace minidb {

static String json_escape(const String& in) {
    String out;
    for (u32 i = 0; i < in.size(); i++) {
        char c = in[i];
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

const char* plan_node_type_name(PlanNodeType type) {
    switch (type) {
        case PlanNodeType::kOneRow: return "OneRow";
        case PlanNodeType::kSeqScan: return "SeqScan";
        case PlanNodeType::kInsert: return "Insert";
        case PlanNodeType::kDelete: return "Delete";
        case PlanNodeType::kUpdate: return "Update";
        case PlanNodeType::kFilter: return "Filter";
        case PlanNodeType::kProject: return "Project";
        case PlanNodeType::kJoin: return "Join";
        case PlanNodeType::kLimit: return "Limit";
        case PlanNodeType::kSort: return "Sort";
        case PlanNodeType::kDistinct: return "Distinct";
        case PlanNodeType::kAggregate: return "Aggregate";
        case PlanNodeType::kUnion: return "Union";
        case PlanNodeType::kIndexScan: return "IndexScan";
        case PlanNodeType::kIndexOnlyScan: return "IndexOnlyScan";
    }
    return "Unknown";
}

static void assign_ids(PlanNode* plan, u32* next_id) {
    if (!plan) return;
    plan->trace_id = (*next_id)++;
    switch (plan->type) {
        case PlanNodeType::kFilter:
            assign_ids(static_cast<FilterPlan*>(plan)->child.get(), next_id);
            break;
        case PlanNodeType::kProject:
            assign_ids(static_cast<ProjectPlan*>(plan)->child.get(), next_id);
            break;
        case PlanNodeType::kDelete:
            assign_ids(static_cast<DeletePlan*>(plan)->child.get(), next_id);
            break;
        case PlanNodeType::kUpdate:
            assign_ids(static_cast<UpdatePlan*>(plan)->child.get(), next_id);
            break;
        case PlanNodeType::kJoin:
            assign_ids(static_cast<JoinPlan*>(plan)->left.get(), next_id);
            assign_ids(static_cast<JoinPlan*>(plan)->right.get(), next_id);
            break;
        case PlanNodeType::kLimit:
            assign_ids(static_cast<LimitPlan*>(plan)->child.get(), next_id);
            break;
        case PlanNodeType::kSort:
            assign_ids(static_cast<SortPlan*>(plan)->child.get(), next_id);
            break;
        case PlanNodeType::kDistinct:
            assign_ids(static_cast<DistinctPlan*>(plan)->child.get(), next_id);
            break;
        case PlanNodeType::kAggregate:
            assign_ids(static_cast<AggregatePlan*>(plan)->child.get(), next_id);
            break;
        case PlanNodeType::kUnion:
            assign_ids(static_cast<UnionPlan*>(plan)->left.get(), next_id);
            assign_ids(static_cast<UnionPlan*>(plan)->right.get(), next_id);
            break;
        default:
            break;
    }
}

void assign_trace_node_ids(PlanNode* plan) {
    u32 next_id = 1;
    assign_ids(plan, &next_id);
}

static const TraceOperatorStats* find_op(const TraceContext& trace, u32 node_id) {
    for (const auto& s : trace.operators()) {
        if (s.node_id == node_id) return &s;
    }
    return nullptr;
}

TraceOptions trace_options_from_statement(u8 level, u32 channels,
                                          const String& events_path) {
    TraceOptions options;
    if (level == 0) options.level = TraceLevel::kSummary;
    else if (level == 2) options.level = TraceLevel::kVerbose;
    else if (level >= 3) options.level = TraceLevel::kDebug;
    else options.level = TraceLevel::kNormal;
    options.channels = channels == 0 ? kTraceDefaultChannels : channels;
    if (options.level == TraceLevel::kVerbose) {
        options.channels |= kTraceMvcc | kTraceIndex;
    }
    if (options.level == TraceLevel::kDebug) {
        options.channels |= kTraceEvent;
    }
    options.events_path = events_path;
    options.include_events = options.level == TraceLevel::kDebug && events_path.empty();
    options.max_events = options.level == TraceLevel::kDebug ? 10000 : 0;
    return options;
}

static const char* level_name(TraceLevel level) {
    switch (level) {
        case TraceLevel::kSummary: return "summary";
        case TraceLevel::kNormal: return "normal";
        case TraceLevel::kVerbose: return "verbose";
        case TraceLevel::kDebug: return "debug";
    }
    return "normal";
}

static void append_channels_json(String* out, u32 channels) {
    struct Ch { u32 bit; const char* name; };
    static const Ch all[] = {
        {kTracePipeline, "pipeline"}, {kTracePlan, "plan"},
        {kTraceExecutor, "executor"}, {kTraceStorage, "storage"},
        {kTraceMvcc, "mvcc"}, {kTraceIndex, "index"},
        {kTraceWal, "wal"}, {kTraceLock, "lock"},
        {kTraceMemory, "memory"}, {kTraceEvent, "event"},
    };
    *out += "[";
    bool first = true;
    for (const auto& ch : all) {
        if ((channels & ch.bit) == 0) continue;
        if (!first) *out += ",";
        *out += "\"";
        *out += ch.name;
        *out += "\"";
        first = false;
    }
    *out += "]";
}

static void append_plan_json(String* out, const PlanNode* plan, const TraceContext& trace) {
    if (!plan) {
        *out += "null";
        return;
    }
    char buf[512];
    const TraceOperatorStats* op = find_op(trace, plan->trace_id);
    std::snprintf(buf, sizeof(buf),
                  "{\"node_id\":%u,\"type\":\"%s\",\"estimated_rows\":%.3f,"
                  "\"startup_cost\":%.3f,\"total_cost\":%.3f,\"actual_rows\":%llu",
                  plan->trace_id, plan_node_type_name(plan->type), plan->plan_rows,
                  plan->startup_cost, plan->total_cost,
                  static_cast<unsigned long long>(op ? op->output_rows : 0));
    *out += buf;
    if (!plan->optimizer_note.empty()) {
        *out += ",\"optimizer_note\":\"";
        *out += json_escape(plan->optimizer_note);
        *out += "\"";
    }
    *out += ",\"children\":[";
    bool first = true;
    auto child = [&](const PlanNode* c) {
        if (!first) *out += ",";
        append_plan_json(out, c, trace);
        first = false;
    };
    switch (plan->type) {
        case PlanNodeType::kFilter: child(static_cast<const FilterPlan*>(plan)->child.get()); break;
        case PlanNodeType::kProject: child(static_cast<const ProjectPlan*>(plan)->child.get()); break;
        case PlanNodeType::kDelete: child(static_cast<const DeletePlan*>(plan)->child.get()); break;
        case PlanNodeType::kUpdate: child(static_cast<const UpdatePlan*>(plan)->child.get()); break;
        case PlanNodeType::kJoin:
            child(static_cast<const JoinPlan*>(plan)->left.get());
            child(static_cast<const JoinPlan*>(plan)->right.get());
            break;
        case PlanNodeType::kLimit: child(static_cast<const LimitPlan*>(plan)->child.get()); break;
        case PlanNodeType::kSort: child(static_cast<const SortPlan*>(plan)->child.get()); break;
        case PlanNodeType::kDistinct: child(static_cast<const DistinctPlan*>(plan)->child.get()); break;
        case PlanNodeType::kAggregate: child(static_cast<const AggregatePlan*>(plan)->child.get()); break;
        case PlanNodeType::kUnion:
            child(static_cast<const UnionPlan*>(plan)->left.get());
            child(static_cast<const UnionPlan*>(plan)->right.get());
            break;
        default:
            break;
    }
    *out += "]}";
}

String build_trace_json(const String& sql, const PlanNode* plan,
                        const TraceContext& trace, const TraceTimings& timings,
                        const char* error) {
    const TraceOptions& options = trace.options();
    String out = "{";
    out += "\"format\":\"minidb.trace.v2\",";
    out += "\"trace\":{\"level\":\"";
    out += level_name(options.level);
    out += "\",\"channels\":";
    append_channels_json(&out, options.channels);
    out += ",\"events_included\":";
    out += options.include_events ? "true" : "false";
    out += ",\"events_ref\":";
    if (options.events_path.empty()) out += "null";
    else {
        out += "\"";
        out += json_escape(options.events_path);
        out += "\"";
    }
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  ",\"events_recorded\":%llu,\"events_dropped\":%llu},",
                  static_cast<unsigned long long>(options.events_recorded),
                  static_cast<unsigned long long>(options.events_dropped));
    out += buf;
    out += "\"query\":{\"sql\":\"";
    out += json_escape(sql);
    out += "\",\"read_only\":true},";
    std::snprintf(buf, sizeof(buf),
                  "\"pipeline\":{\"plan_us\":%llu,\"executor_create_us\":%llu,"
                  "\"execute_us\":%llu,\"actual_rows\":%llu},",
                  static_cast<unsigned long long>(timings.plan_us),
                  static_cast<unsigned long long>(timings.executor_create_us),
                  static_cast<unsigned long long>(timings.execute_us),
                  static_cast<unsigned long long>(timings.actual_rows));
    out += buf;
    out += "\"error\":";
    if (error) {
        out += "\"";
        out += json_escape(String(error));
        out += "\"";
    } else {
        out += "null";
    }
    double hit_rate = trace.buffer_fetches() == 0
        ? 0.0
        : static_cast<double>(trace.buffer_hits()) / static_cast<double>(trace.buffer_fetches());
    std::snprintf(buf, sizeof(buf),
                  ",\"summary\":{\"execution_ms\":%.3f,\"rows\":%llu,"
                  "\"buffer_hit_rate\":%.6f,\"buffer_fetches\":%llu,\"buffer_hits\":%llu,"
                  "\"buffer_misses\":%llu,\"buffer_new_pages\":%llu,"
                  "\"buffer_dirty_marks\":%llu,\"buffer_flush_pages\":%llu,"
                  "\"buffer_evictions\":%llu,\"wal_records\":%llu,\"wal_bytes\":%llu,"
                  "\"lock_acquires\":%llu,\"lock_waits\":%llu,\"lock_wait_us\":%llu},",
                  static_cast<double>(timings.execute_us) / 1000.0,
                  static_cast<unsigned long long>(timings.actual_rows),
                  hit_rate,
                  static_cast<unsigned long long>(trace.buffer_fetches()),
                  static_cast<unsigned long long>(trace.buffer_hits()),
                  static_cast<unsigned long long>(trace.buffer_misses()),
                  static_cast<unsigned long long>(trace.buffer_new_pages()),
                  static_cast<unsigned long long>(trace.buffer_dirty_marks()),
                  static_cast<unsigned long long>(trace.buffer_flush_pages()),
                  static_cast<unsigned long long>(trace.buffer_evictions()),
                  static_cast<unsigned long long>(trace.wal_records()),
                  static_cast<unsigned long long>(trace.wal_bytes()),
                  static_cast<unsigned long long>(trace.lock_acquires()),
                  static_cast<unsigned long long>(trace.lock_waits()),
                  static_cast<unsigned long long>(trace.lock_wait_us()));
    out += buf;
    out += "\"hotspots\":{\"slowest_operators\":[";
    std::vector<const TraceOperatorStats*> ops;
    for (const auto& s : trace.operators()) if (s.node_id != 0) ops.push_back(&s);
    for (size_t i = 0; i < ops.size(); i++) {
        for (size_t j = i + 1; j < ops.size(); j++) {
            if (ops[j]->next_us > ops[i]->next_us) {
                const TraceOperatorStats* tmp = ops[i]; ops[i] = ops[j]; ops[j] = tmp;
            }
        }
    }
    bool first = true;
    for (size_t i = 0; i < ops.size() && i < 5; i++) {
        if (!first) out += ",";
        std::snprintf(buf, sizeof(buf),
                      "{\"node_id\":%u,\"executor\":\"%s\",\"next_us\":%llu,\"rows\":%llu}",
                      ops[i]->node_id, ops[i]->executor_type.c_str(),
                      static_cast<unsigned long long>(ops[i]->next_us),
                      static_cast<unsigned long long>(ops[i]->output_rows));
        out += buf;
        first = false;
    }
    out += "]},";
    if (options.channel_enabled(kTracePlan) && options.level != TraceLevel::kSummary) {
        out += "\"plan\":";
        append_plan_json(&out, plan, trace);
        out += ",";
    }
    out += "\"operators\":[";
    first = true;
    for (const auto& s : trace.operators()) {
        if (s.node_id == 0) continue;
        if (!first) out += ",";
        std::snprintf(buf, sizeof(buf),
                      "{\"node_id\":%u,\"executor\":\"%s\",\"init_calls\":%llu,"
                      "\"next_calls\":%llu,\"output_rows\":%llu,\"init_us\":%llu,"
                      "\"next_us\":%llu,\"buffer_fetches\":%llu,\"buffer_hits\":%llu,"
                      "\"buffer_misses\":%llu,\"buffer_new_pages\":%llu,"
                      "\"buffer_dirty_marks\":%llu,\"buffer_flush_pages\":%llu,"
                      "\"buffer_evictions\":%llu,\"heap_tuples_seen\":%llu,"
                      "\"heap_tuples_visible\":%llu,\"heap_tuples_invisible\":%llu,"
                      "\"heap_tuples_filtered\":%llu,\"version_chain_steps\":%llu,"
                      "\"index_keys_examined\":%llu,\"index_rids_returned\":%llu,"
                      "\"index_heap_rechecks\":%llu,\"wal_records\":%llu,"
                      "\"wal_bytes\":%llu,\"lock_acquires\":%llu,\"lock_waits\":%llu,"
                      "\"lock_wait_us\":%llu}",
                      s.node_id, s.executor_type.c_str(),
                      static_cast<unsigned long long>(s.init_calls),
                      static_cast<unsigned long long>(s.next_calls),
                      static_cast<unsigned long long>(s.output_rows),
                      static_cast<unsigned long long>(s.init_us),
                      static_cast<unsigned long long>(s.next_us),
                      static_cast<unsigned long long>(s.buffer_fetches),
                      static_cast<unsigned long long>(s.buffer_hits),
                      static_cast<unsigned long long>(s.buffer_misses),
                      static_cast<unsigned long long>(s.buffer_new_pages),
                      static_cast<unsigned long long>(s.buffer_dirty_marks),
                      static_cast<unsigned long long>(s.buffer_flush_pages),
                      static_cast<unsigned long long>(s.buffer_evictions),
                      static_cast<unsigned long long>(s.heap_tuples_seen),
                      static_cast<unsigned long long>(s.heap_tuples_visible),
                      static_cast<unsigned long long>(s.heap_tuples_invisible),
                      static_cast<unsigned long long>(s.heap_tuples_filtered),
                      static_cast<unsigned long long>(s.version_chain_steps),
                      static_cast<unsigned long long>(s.index_keys_examined),
                      static_cast<unsigned long long>(s.index_rids_returned),
                      static_cast<unsigned long long>(s.index_heap_rechecks),
                      static_cast<unsigned long long>(s.wal_records),
                      static_cast<unsigned long long>(s.wal_bytes),
                      static_cast<unsigned long long>(s.lock_acquires),
                      static_cast<unsigned long long>(s.lock_waits),
                      static_cast<unsigned long long>(s.lock_wait_us));
        out += buf;
        first = false;
    }
    out += "]";
    if (options.include_events) {
        out += ",\"events\":[";
        first = true;
        for (const auto& e : trace.events()) {
            if (!first) out += ",";
            std::snprintf(buf, sizeof(buf),
                          "{\"ts_us\":%llu,\"node_id\":%u,\"channel\":\"%s\",\"event\":\"%s\",\"detail\":",
                          static_cast<unsigned long long>(e.ts_us), e.node_id,
                          e.phase.c_str(), e.name.c_str());
            out += buf;
            if (e.detail.empty()) {
                out += "null";
            } else {
                out += e.detail;
            }
            out += "}";
            first = false;
        }
        out += "]";
    }
    out += "}";
    return out;
}

} // namespace minidb
