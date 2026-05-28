#pragma once

#include "common/trace.h"
#include "container/string.h"
#include "sql/planner/plan_node.h"

namespace minidb {

struct TraceTimings {
    u64 plan_us = 0;
    u64 executor_create_us = 0;
    u64 execute_us = 0;
    u64 actual_rows = 0;
};

const char* plan_node_type_name(PlanNodeType type);
void assign_trace_node_ids(PlanNode* plan);
String build_trace_json(const String& sql, const PlanNode* plan,
                        const TraceContext& trace, const TraceTimings& timings,
                        const char* error);
TraceOptions trace_options_from_statement(u8 level, u32 channels,
                                          const String& events_path);

} // namespace minidb
