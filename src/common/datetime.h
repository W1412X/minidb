/**
 * @file datetime.h
 * @brief UTC datetime/timestamp parsing and formatting helpers.
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"

namespace minidb {

// Parse a UTC datetime string into microseconds since 1970-01-01 00:00:00.
// Accepted forms:
//   YYYY-MM-DD
//   YYYY-MM-DD HH:MM:SS[.ffffff]
//   YYYY-MM-DDTHH:MM:SS[.ffffff]
// Time zone suffixes are intentionally rejected; MiniDB stores UTC values and
// does not manage session time zones.
bool datetime_parse_utc(const char* text, i64* out_micros);

// Format microseconds since epoch as YYYY-MM-DD HH:MM:SS[.ffffff].
String datetime_format_utc(i64 micros);

} // namespace minidb
