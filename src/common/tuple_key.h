/**
 * @file tuple_key.h
 * @brief Stable tuple/value key encoding for hash-based executor operators.
 */
#pragma once

#include "container/string.h"
#include "container/vector.h"
#include "record/tuple.h"
#include "record/value.h"
#include <cstdio>

namespace minidb {

inline String encode_value_key(const Value& value) {
    String out;
    out += String(static_cast<u64>(static_cast<u32>(value.type_id())));
    out += ':';
    if (value.is_null()) {
        out += "NULL";
        out += ';';
        return out;
    }

    String text;
    char buf[64];
    switch (value.type_id()) {
        case TypeId::kBool:
            text = value.get_bool() ? "1" : "0";
            break;
        case TypeId::kInt32:
            std::snprintf(buf, sizeof(buf), "%d", value.get_int32());
            text = buf;
            break;
        case TypeId::kInt64:
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.get_int64()));
            text = buf;
            break;
        case TypeId::kTimestamp:
        case TypeId::kDatetime:
            std::snprintf(buf, sizeof(buf), "%lld",
                          static_cast<long long>(value.get_datetime_micros()));
            text = buf;
            break;
        case TypeId::kFloat:
            std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value.get_float()));
            text = buf;
            break;
        case TypeId::kDouble:
            std::snprintf(buf, sizeof(buf), "%.17g", value.get_double());
            text = buf;
            break;
        case TypeId::kVarchar:
            text = value.get_string();
            break;
        case TypeId::kNull:
            text = "NULL";
            break;
    }
    out += String(static_cast<u64>(text.size()));
    out += '#';
    out += text;
    out += ';';
    return out;
}

inline String make_values_key(const Vector<Value>& values) {
    String out;
    out += String(static_cast<u64>(values.size()));
    out += '|';
    for (u32 i = 0; i < values.size(); i++) {
        out += encode_value_key(values[i]);
    }
    return out;
}

inline String make_tuple_key(const Tuple& tuple) {
    String out;
    out += String(static_cast<u64>(tuple.column_count()));
    out += '|';
    for (u32 i = 0; i < tuple.column_count(); i++) {
        out += encode_value_key(tuple.get_value(i));
    }
    return out;
}

inline bool make_projected_tuple_key(const Tuple& tuple, const Vector<u32>& columns,
                                     bool skip_if_any_null, String* out) {
    if (!out) return false;
    out->clear();
    *out += String(static_cast<u64>(columns.size()));
    *out += '|';
    for (u32 i = 0; i < columns.size(); i++) {
        if (columns[i] >= tuple.column_count()) return false;
        Value value = tuple.get_value(columns[i]);
        if (skip_if_any_null && value.is_null()) return false;
        *out += encode_value_key(value);
    }
    return true;
}

inline bool make_projected_values_key(const Vector<Value>& values, const Vector<u32>& columns,
                                      bool skip_if_any_null, String* out) {
    if (!out) return false;
    out->clear();
    *out += String(static_cast<u64>(columns.size()));
    *out += '|';
    for (u32 i = 0; i < columns.size(); i++) {
        if (columns[i] >= values.size()) return false;
        if (skip_if_any_null && values[columns[i]].is_null()) return false;
        *out += encode_value_key(values[columns[i]]);
    }
    return true;
}

} // namespace minidb
