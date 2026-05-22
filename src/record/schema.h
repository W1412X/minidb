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
    // PostgreSQL-style logical column deletion. When true the column is
    // invisible to user queries (SELECT *, column name lookup, INSERT
    // implicit column list, DESC TABLE) but the physical tuple slot is
    // preserved — existing data stays untouched, new rows store NULL in
    // the dropped slot. This makes ALTER TABLE DROP COLUMN a metadata-only
    // O(1) operation and fully transactional (rollback just clears the flag).
    bool   is_dropped;
    // Declared maximum length for VARCHAR(n). 0 means "unbounded" — either
    // TEXT or VARCHAR with no length (lexer parses both into kVarchar).
    // Stored as u32 so it round-trips through schema serialisation; the
    // parser feeds in an i32 and only positive values are kept.
    u32    varchar_length;
    String default_value;
    // SQL text of a column-level CHECK constraint, without the surrounding
    // parentheses. Empty when the column has no CHECK. Re-parsed at INSERT
    // / UPDATE time so the catalog stays a pure string-keyed store and
    // avoids carrying parser AST through serialisation.
    String check_expr;

    Column() : type(TypeId::kNull), not_null(false), is_primary(false),
               is_unique(false), is_dropped(false), varchar_length(0) {}
    Column(const String& n, TypeId t, bool nn = false, bool pk = false)
        : name(n), type(t), not_null(nn), is_primary(pk),
          is_unique(false), is_dropped(false), varchar_length(0) {}

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
    // Number of user-visible (non-dropped) columns.
    u32 visible_column_count() const;
    bool has_dropped_columns() const;
    bool empty() const { return columns_.empty(); }

    // Check a row against per-column constraints. Returns nullptr when the
    // row is acceptable; otherwise returns a static error string suitable for
    // set_executor_error. Currently enforces:
    //   - column count match,
    //   - NOT NULL,
    //   - VARCHAR(n) length bound (when n > 0).
    // CHECK constraints will plug in here when implemented.
    const char* validate_row(const Vector<Value>& row) const;

    // Serialize
    byte* serialize(byte* buf) const;
    static Schema deserialize(const byte* buf);
    static Schema deserialize(const byte* buf, u32 length);
    u32 serialized_size() const;

private:
    Vector<Column> columns_;
};

} // namespace minidb
