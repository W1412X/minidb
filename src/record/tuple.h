/**
 * @file tuple.h
 * @brief Tuple — One row, = Schema + Values + MVCC header + version chain
 *
 * In-page serialization format:
 *   [xmin 8B] [xmax 8B] [next_page 8B] [next_slot 2B] [num_cols 4B] [null_bitmap] [values...]
 */
#pragma once

#include "common/defs.h"
#include "container/vector.h"
#include "record/value.h"
#include "record/schema.h"

namespace minidb {

static constexpr u64 kInvalidPageId = 0;

class Tuple {
public:
    Tuple();
    Tuple(const Schema& schema, const Vector<Value>& values);

    // MVCC fields
    u64 xmin() const { return xmin_; }
    u64 xmax() const { return xmax_; }
    void set_xmin(u64 val) { xmin_ = val; }
    void set_xmax(u64 val) { xmax_ = val; }

    // Version chain
    PageId next_version_page() const { return next_ver_page_; }
    SlotIdx next_version_slot() const { return next_ver_slot_; }
    void set_next_version(PageId page, SlotIdx slot) {
        next_ver_page_ = page;
        next_ver_slot_ = slot;
    }
    bool has_next_version() const { return next_ver_page_ != kNullPageId; }

    // Value access
    Value get_value(u32 col_idx) const;
    void set_value(u32 col_idx, const Value& val);
    u32 column_count() const { return values_.size(); }
    const Schema& schema() const { return *schema_; }
    const Vector<Value>& values() const { return values_; }

    // Serialize to in-page format
    byte* serialize_to_page(byte* buf) const;
    static Tuple deserialize_from_page(const byte* buf, const Schema& schema);
    static Tuple deserialize_from_page(const byte* buf, const Schema& schema, u32 length);
    static Tuple deserialize_projected_from_page(const byte* buf,
                                                 const Schema& source_schema,
                                                 const Schema& output_schema,
                                                 const Vector<u32>& projected_columns,
                                                 u32 length);

    u32 serialized_size() const;

    // Compare
    int compare(const Tuple& other, u32 col_idx) const;

private:
    u64 xmin_;
    u64 xmax_;
    PageId next_ver_page_;
    SlotIdx next_ver_slot_;
    Vector<Value> values_;
    const Schema* schema_;
};

} // namespace minidb
