/**
 * @file tuple.cpp
 * @brief Tuple implementation — includes version chain
 *
 * Serialization format:
 *   [xmin 8B][xmax 8B][next_page 8B][next_slot 2B][num_cols 4B][null_bitmap][values]
 */
#include "record/tuple.h"
#include "common/config.h"
#include <cstring>

namespace minidb {

Tuple::Tuple() : xmin_(0), xmax_(0), next_ver_page_(kNullPageId),
                 next_ver_slot_(0), schema_(nullptr) {}

Tuple::Tuple(const Schema& schema, const Vector<Value>& values)
    : xmin_(0), xmax_(0), next_ver_page_(kNullPageId),
      next_ver_slot_(0), values_(values), schema_(&schema) {}

Value Tuple::get_value(u32 col_idx) const {
    if (col_idx >= values_.size()) return Value();
    return values_[col_idx];
}

void Tuple::set_value(u32 col_idx, const Value& val) {
    if (col_idx < values_.size()) {
        values_[col_idx] = val;
    }
}

// ============================================================
// Serialize: [xmin 8B][xmax 8B][next_page 8B][next_slot 2B][num_cols 4B][null_bitmap][values]
// ============================================================

static constexpr u32 kTupleHeaderSize = 8 + 8 + 8 + 2 + 4;  // 30 bytes

byte* Tuple::serialize_to_page(byte* buf) const {
    // xmin
    std::memcpy(buf, &xmin_, 8);
    buf += 8;
    // xmax
    std::memcpy(buf, &xmax_, 8);
    buf += 8;
    // next_version
    std::memcpy(buf, &next_ver_page_, 8);
    buf += 8;
    std::memcpy(buf, &next_ver_slot_, 2);
    buf += 2;
    // num_cols
    u32 num_cols = values_.size();
    std::memcpy(buf, &num_cols, 4);
    buf += 4;

    // null bitmap
    u32 bitmap_size = (num_cols + 7) / 8;
    std::memset(buf, 0, bitmap_size);
    for (u32 i = 0; i < num_cols; i++) {
        if (values_[i].is_null()) {
            buf[i / 8] |= (1 << (i % 8));
        }
    }
    buf += bitmap_size;

    // values
    for (u32 i = 0; i < num_cols; i++) {
        if (!values_[i].is_null()) {
            buf = values_[i].serialize(buf);
        }
    }

    return buf;
}

static bool read_value_bounded(const byte*& buf, const byte* end, Value* out) {
    if (!out || buf >= end) return false;
    TypeId type = static_cast<TypeId>(*buf);
    buf++;
    switch (type) {
        case TypeId::kBool:
            if (buf + 1 > end) return false;
            *out = Value(*buf == 1);
            buf += 1;
            return true;
        case TypeId::kInt32: {
            if (buf + 4 > end) return false;
            i32 v;
            std::memcpy(&v, buf, 4);
            *out = Value(v);
            buf += 4;
            return true;
        }
        case TypeId::kInt64: {
            if (buf + 8 > end) return false;
            i64 v;
            std::memcpy(&v, buf, 8);
            *out = Value(v);
            buf += 8;
            return true;
        }
        case TypeId::kTimestamp: {
            if (buf + 8 > end) return false;
            i64 v;
            std::memcpy(&v, buf, 8);
            *out = Value::timestamp(v);
            buf += 8;
            return true;
        }
        case TypeId::kDatetime: {
            if (buf + 8 > end) return false;
            i64 v;
            std::memcpy(&v, buf, 8);
            *out = Value::datetime(v);
            buf += 8;
            return true;
        }
        case TypeId::kFloat: {
            if (buf + 4 > end) return false;
            float v;
            std::memcpy(&v, buf, 4);
            *out = Value(v);
            buf += 4;
            return true;
        }
        case TypeId::kDouble: {
            if (buf + 8 > end) return false;
            double v;
            std::memcpy(&v, buf, 8);
            *out = Value(v);
            buf += 8;
            return true;
        }
        case TypeId::kVarchar: {
            if (buf + 4 > end) return false;
            u32 len;
            std::memcpy(&len, buf, 4);
            buf += 4;
            // Compare against the remaining byte count, NOT `buf + len > end`:
            // `len` is attacker/corruption-controlled, so `buf + len` can
            // overflow the pointer (UB) and wrap below `end`, bypassing the
            // guard and reading gigabytes out of bounds.
            if (len > static_cast<u32>(end - buf)) return false;
            *out = Value(String(reinterpret_cast<const char*>(buf), len));
            buf += len;
            return true;
        }
        case TypeId::kNull:
            *out = Value();
            return true;
        default:
            return false;
    }
}

static bool skip_value_bounded(const byte*& buf, const byte* end) {
    if (buf >= end) return false;
    TypeId type = static_cast<TypeId>(*buf);
    buf++;
    switch (type) {
        case TypeId::kBool:
            if (buf + 1 > end) return false;
            buf += 1;
            return true;
        case TypeId::kInt32:
        case TypeId::kFloat:
            if (buf + 4 > end) return false;
            buf += 4;
            return true;
        case TypeId::kInt64:
        case TypeId::kTimestamp:
        case TypeId::kDatetime:
        case TypeId::kDouble:
            if (buf + 8 > end) return false;
            buf += 8;
            return true;
        case TypeId::kVarchar: {
            if (buf + 4 > end) return false;
            u32 len;
            std::memcpy(&len, buf, 4);
            buf += 4;
            // See read_value_bounded: avoid pointer-overflow bypass of the bound.
            if (len > static_cast<u32>(end - buf)) return false;
            buf += len;
            return true;
        }
        case TypeId::kNull:
            return true;
        default:
            return false;
    }
}

Tuple Tuple::deserialize_from_page(const byte* buf, const Schema& schema) {
    return deserialize_from_page(buf, schema, kPageSize);
}

Tuple Tuple::deserialize_from_page(const byte* buf, const Schema& schema, u32 length) {
    Tuple tuple;
    tuple.schema_ = &schema;
    if (!buf || length < kTupleHeaderSize) return tuple;
    const byte* end = buf + length;

    // xmin
    std::memcpy(&tuple.xmin_, buf, 8);
    buf += 8;
    // xmax
    std::memcpy(&tuple.xmax_, buf, 8);
    buf += 8;
    // next_version
    std::memcpy(&tuple.next_ver_page_, buf, 8);
    buf += 8;
    std::memcpy(&tuple.next_ver_slot_, buf, 2);
    buf += 2;
    // num_cols
    u32 num_cols;
    std::memcpy(&num_cols, buf, 4);
    buf += 4;
    static constexpr u32 kMaxTupleColumns = 4096;
    if (num_cols > kMaxTupleColumns) {
        tuple.values_.clear();
        return tuple;
    }

    // null bitmap
    u32 bitmap_size = (num_cols + 7) / 8;
    if (buf + bitmap_size > end) {
        tuple.values_.clear();
        return tuple;
    }
    const byte* bitmap = buf;
    buf += bitmap_size;

    // values — reserve up front so the per-column push_back doesn't realloc
    // the underlying buffer repeatedly. For a 5-column schema this saves up
    // to log2(5)+1 = 4 reallocations + memcpy round-trips per tuple.
    tuple.values_.clear();
    u32 reserve_n = num_cols < schema.column_count() ? num_cols : schema.column_count();
    tuple.values_.reserve(reserve_n);
    for (u32 i = 0; i < num_cols; i++) {
        if (bitmap[i / 8] & (1 << (i % 8))) {
            if (i < schema.column_count()) {
                tuple.values_.push_back(Value());
            }
        } else {
            Value val;
            if (!read_value_bounded(buf, end, &val)) {
                tuple.values_.clear();
                return tuple;
            }
            if (i < schema.column_count()) {
                tuple.values_.push_back(static_cast<Value&&>(val));
            }
        }
    }
    // Pad missing columns (from ADD COLUMN) with default values
    while (tuple.values_.size() < schema.column_count()) {
        u32 col_idx = static_cast<u32>(tuple.values_.size());
        tuple.values_.push_back(schema.get_column(col_idx).default_as_value());
    }

    return tuple;
}

Tuple Tuple::deserialize_projected_from_page(const byte* buf,
                                             const Schema& source_schema,
                                             const Schema& output_schema,
                                             const Vector<u32>& projected_columns,
                                             u32 length) {
    if (projected_columns.empty()) {
        return deserialize_from_page(buf, output_schema, length);
    }

    Tuple tuple;
    tuple.schema_ = &output_schema;
    if (!buf || length < kTupleHeaderSize) return tuple;
    const byte* end = buf + length;

    std::memcpy(&tuple.xmin_, buf, 8);
    buf += 8;
    std::memcpy(&tuple.xmax_, buf, 8);
    buf += 8;
    std::memcpy(&tuple.next_ver_page_, buf, 8);
    buf += 8;
    std::memcpy(&tuple.next_ver_slot_, buf, 2);
    buf += 2;
    u32 num_cols;
    std::memcpy(&num_cols, buf, 4);
    buf += 4;
    static constexpr u32 kMaxTupleColumns = 4096;
    if (num_cols > kMaxTupleColumns) return tuple;

    u32 bitmap_size = (num_cols + 7) / 8;
    if (buf + bitmap_size > end) return tuple;
    const byte* bitmap = buf;
    buf += bitmap_size;

    // Build a direct source-col -> output-pos map up front (O(K) once) so the
    // per-column needed/output-pos lookup inside the hot decode loop is O(1)
    // instead of O(K) via column_projected(). For tables with N columns and
    // K projected columns, this turns the loop's total work from O(N×K)
    // into O(N+K). Stack-allocated, no heap allocation in the hot path.
    static constexpr u32 kMaxStackMap = 128;
    i32 stack_map[kMaxStackMap];
    Vector<i32> heap_map;
    i32* source_to_output = nullptr;
    if (num_cols <= kMaxStackMap) {
        source_to_output = stack_map;
    } else {
        heap_map.resize(num_cols);
        source_to_output = heap_map.data();
    }
    for (u32 i = 0; i < num_cols; i++) source_to_output[i] = -1;
    for (u32 i = 0; i < projected_columns.size(); i++) {
        u32 src = projected_columns[i];
        if (src < num_cols) source_to_output[src] = static_cast<i32>(i);
    }

    tuple.values_.clear();
    tuple.values_.reserve(projected_columns.size());
    for (u32 i = 0; i < projected_columns.size(); i++) tuple.values_.push_back(Value());

    for (u32 i = 0; i < num_cols; i++) {
        i32 out_pos = source_to_output[i];
        bool needed = out_pos >= 0;
        bool is_null_col = (bitmap[i / 8] & (1 << (i % 8))) != 0;
        if (is_null_col) {
            // Already initialised to NULL when we filled values_ with Value().
            continue;
        }
        if (needed) {
            Value val;
            if (!read_value_bounded(buf, end, &val)) {
                tuple.values_.clear();
                return tuple;
            }
            tuple.values_[static_cast<u32>(out_pos)] = static_cast<Value&&>(val);
        } else if (!skip_value_bounded(buf, end)) {
            tuple.values_.clear();
            return tuple;
        }
    }

    // Pad any projected columns past the stored num_cols with the source
    // schema's defaults (handles ADD COLUMN backfill).
    for (u32 i = 0; i < projected_columns.size(); i++) {
        u32 source_col = projected_columns[i];
        if (source_col < num_cols || source_col >= source_schema.column_count()) continue;
        const Column& col = source_schema.get_column(source_col);
        if (col.has_default()) {
            tuple.values_[i] = col.default_as_value();
        }
    }
    return tuple;
}

bool Tuple::read_header_from_page(const byte* buf, u32 length,
                                  u64* xmin, u64* xmax,
                                  PageId* next_page, SlotIdx* next_slot) {
    if (!buf || length < kTupleHeaderSize) return false;
    if (xmin) std::memcpy(xmin, buf, 8);
    if (xmax) std::memcpy(xmax, buf + 8, 8);
    if (next_page) std::memcpy(next_page, buf + 16, 8);
    if (next_slot) std::memcpy(next_slot, buf + 24, 2);
    return true;
}

bool Tuple::read_column_from_page(const byte* buf, const Schema& schema,
                                  u32 column_idx, u32 length, Value* out) {
    if (!buf || !out || length < kTupleHeaderSize || column_idx >= schema.column_count()) {
        return false;
    }
    const byte* end = buf + length;
    buf += 26;
    u32 num_cols = 0;
    std::memcpy(&num_cols, buf, 4);
    buf += 4;
    if (column_idx >= num_cols) {
        *out = schema.get_column(column_idx).default_as_value();
        return true;
    }
    u32 bitmap_size = (num_cols + 7) / 8;
    if (buf + bitmap_size > end) return false;
    const byte* bitmap = buf;
    buf += bitmap_size;
    for (u32 i = 0; i < num_cols; i++) {
        bool is_null = (bitmap[i / 8] & (1 << (i % 8))) != 0;
        if (i == column_idx) {
            if (is_null) {
                *out = Value();
                return true;
            }
            return read_value_bounded(buf, end, out);
        }
        if (!is_null && !skip_value_bounded(buf, end)) return false;
    }
    return false;
}

u32 Tuple::serialized_size() const {
    u32 num_cols = values_.size();
    u32 size = kTupleHeaderSize;
    size += (num_cols + 7) / 8;
    for (u32 i = 0; i < num_cols; i++) {
        if (!values_[i].is_null()) {
            size += values_[i].serialized_size();
        }
    }
    return size;
}

int Tuple::compare(const Tuple& other, u32 col_idx) const {
    if (col_idx >= values_.size() || col_idx >= other.values_.size()) return 0;
    return values_[col_idx].compare(other.values_[col_idx]);
}

} // namespace minidb
