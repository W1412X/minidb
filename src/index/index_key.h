/**
 * @file index_key.h
 * @brief Composite physical index key with NULL/text-aware comparison.
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include "container/vector.h"
#include "record/value.h"

namespace minidb {

// Maximum encoded size per index key. Determines B+ tree fan-out: smaller
// values pack more keys per page, improving lookup and insert throughput.
// 128 bytes accommodates single-column keys of any type and most composite
// keys (e.g. INT+VARCHAR(100)). Keys exceeding this limit cannot be indexed.
// Previous value (512) limited fan-out to ~13 keys/page; 128 gives ~57.
static constexpr u16 kIndexKeyMaxEncodedSize = 128;

struct IndexKeyColumn {
    TypeId type;
    bool descending;
    bool nulls_first;
    u32 collation_id;

    IndexKeyColumn()
        : type(TypeId::kNull), descending(false), nulls_first(true), collation_id(0) {}
    explicit IndexKeyColumn(TypeId t, bool desc = false, bool nf = true, u32 coll = 0)
        : type(t), descending(desc), nulls_first(nf), collation_id(coll) {}
};

class IndexKeySchema {
public:
    void add_column(TypeId type, bool descending = false, bool nulls_first = true,
                    u32 collation_id = 0);
    u32 column_count() const { return columns_.size(); }
    const IndexKeyColumn& column(u32 idx) const { return columns_[idx]; }

private:
    Vector<IndexKeyColumn> columns_;
};

class IndexKey {
public:
    IndexKey() = default;
    explicit IndexKey(const Value& value);
    explicit IndexKey(const Vector<Value>& values);

    static IndexKey single(const Value& value) { return IndexKey(value); }
    static IndexKey from_values(const Vector<Value>& values) { return IndexKey(values); }

    u32 column_count() const { return values_.size(); }
    bool empty() const { return values_.empty(); }
    bool is_null() const { return values_.empty(); }
    const Value& value(u32 idx) const { return values_[idx]; }
    const Vector<Value>& values() const { return values_; }
    Value first_value() const { return values_.empty() ? Value() : values_[0]; }

    int compare(const IndexKey& other, const IndexKeySchema* schema = nullptr) const;
    bool starts_with(const IndexKey& prefix, const IndexKeySchema* schema = nullptr) const;

    bool operator==(const IndexKey& other) const { return compare(other) == 0; }
    bool operator!=(const IndexKey& other) const { return compare(other) != 0; }
    bool operator<(const IndexKey& other) const { return compare(other) < 0; }
    bool operator>(const IndexKey& other) const { return compare(other) > 0; }
    bool operator<=(const IndexKey& other) const { return compare(other) <= 0; }
    bool operator>=(const IndexKey& other) const { return compare(other) >= 0; }

    bool encode(byte* slot, u32 slot_size = kIndexKeyMaxEncodedSize) const;
    static IndexKey decode(const byte* slot, u32 slot_size = kIndexKeyMaxEncodedSize);
    u32 encoded_size() const;
    bool fits(u32 slot_size = kIndexKeyMaxEncodedSize) const;
    String to_string() const;

private:
    Vector<Value> values_;
};

} // namespace minidb
