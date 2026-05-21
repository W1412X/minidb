/**
 * @file schema.h
 * @brief Schema — Column definitions for a table
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include "container/vector.h"
#include "record/value.h"

namespace minidb {

struct Column {
    String name;
    String table_name;
    TypeId type;
    bool   not_null;
    bool   is_primary;
    bool   is_unique;
    String default_value;

    Column() : type(TypeId::kNull), not_null(false), is_primary(false), is_unique(false) {}
    Column(const String& n, TypeId t, bool nn = false, bool pk = false)
        : name(n), type(t), not_null(nn), is_primary(pk), is_unique(false) {}

    // Parse the textual default into a Value of this column's type. Returns
    // an explicit NULL Value when no default is declared so callers can
    // distinguish "no default" from a literal NULL default.
    bool has_default() const { return !default_value.empty(); }
    Value default_as_value() const;
};

class Schema {
public:
    Schema() = default;

    void add_column(const Column& col);
    void remove_column(u32 idx);
    void rename_column(u32 idx, const String& new_name);
    const Column& get_column(u32 idx) const;
    int get_column_index(const String& name) const;
    int get_column_index(const String& table, const String& column) const;
    u32 column_count() const { return columns_.size(); }
    bool empty() const { return columns_.empty(); }

    // Serialize
    byte* serialize(byte* buf) const;
    static Schema deserialize(const byte* buf);
    static Schema deserialize(const byte* buf, u32 length);
    u32 serialized_size() const;

private:
    Vector<Column> columns_;
};

} // namespace minidb
