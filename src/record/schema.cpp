/**
 * @file schema.cpp
 * @brief Schema implementation
 */
#include "record/schema.h"
#include <cstring>
#include <cstdlib>

namespace minidb {

Value Column::default_as_value() const {
    if (default_value.empty()) return Value();
    switch (type) {
        case TypeId::kInt32:
        case TypeId::kInt64: {
            char* end = nullptr;
            long v = std::strtol(default_value.c_str(), &end, 10);
            return Value(static_cast<i64>(v));
        }
        case TypeId::kFloat:
        case TypeId::kDouble: {
            char* end = nullptr;
            double v = std::strtod(default_value.c_str(), &end);
            return Value(v);
        }
        case TypeId::kBool:
            return Value(default_value == "true" || default_value == "1" ||
                         default_value == "TRUE");
        default:
            return Value(default_value);
    }
}

void Schema::add_column(const Column& col) {
    columns_.push_back(col);
}

u32 Schema::visible_column_count() const {
    u32 count = 0;
    for (u32 i = 0; i < columns_.size(); i++) {
        if (!columns_[i].is_dropped) count++;
    }
    return count;
}

bool Schema::has_dropped_columns() const {
    for (u32 i = 0; i < columns_.size(); i++) {
        if (columns_[i].is_dropped) return true;
    }
    return false;
}

const char* Schema::validate_row(const Vector<Value>& row) const {
    if (row.size() != columns_.size()) {
        return "row column count does not match table schema";
    }
    for (u32 i = 0; i < columns_.size(); i++) {
        const Column& col = columns_[i];
        // Dropped columns hold NULL — skip constraint checks.
        if (col.is_dropped) continue;
        if (col.not_null && row[i].is_null()) {
            return "NOT NULL constraint violated";
        }
        if (col.type == TypeId::kVarchar && col.varchar_length > 0 &&
            !row[i].is_null() && row[i].get_string().size() > col.varchar_length) {
            return "value too long for declared VARCHAR(n) column";
        }
    }
    return nullptr;
}

void Schema::remove_column(u32 idx) {
    if (idx < columns_.size()) {
        columns_.erase(columns_.begin() + idx);
    }
}

void Schema::rename_column(u32 idx, const String& new_name) {
    if (idx < columns_.size()) {
        columns_[idx].name = new_name;
    }
}

const Column& Schema::get_column(u32 idx) const {
    static const Column kDefaultColumn;
    if (idx >= columns_.size()) return kDefaultColumn;
    return columns_[idx];
}

int Schema::get_column_index(const String& name) const {
    for (u32 i = 0; i < columns_.size(); i++) {
        if (columns_[i].is_dropped) continue;
        if (columns_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int Schema::get_column_index(const String& table, const String& column) const {
    for (u32 i = 0; i < columns_.size(); i++) {
        if (columns_[i].is_dropped) continue;
        if (columns_[i].name == column) {
            if (columns_[i].table_name.empty() || columns_[i].table_name == table) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

byte* Schema::serialize(byte* buf) const {
    u32 n = columns_.size();
    std::memcpy(buf, &n, 4);
    buf += 4;
    for (u32 i = 0; i < n; i++) {
        const Column& col = columns_[i];
        // name
        u16 name_len = col.name.size();
        std::memcpy(buf, &name_len, 2);
        buf += 2;
        std::memcpy(buf, col.name.c_str(), name_len);
        buf += name_len;
        // table_name
        u16 tbl_len = col.table_name.size();
        std::memcpy(buf, &tbl_len, 2);
        buf += 2;
        if (tbl_len > 0) {
            std::memcpy(buf, col.table_name.c_str(), tbl_len);
            buf += tbl_len;
        }
        // type
        *buf = static_cast<byte>(col.type);
        buf++;
        // flags — bit 0: not_null, bit 1: is_primary, bit 2: is_unique,
        //         bit 3: is_dropped (PostgreSQL-style logical deletion)
        byte flags = (col.not_null ? 1 : 0) | (col.is_primary ? 2 : 0) |
                     (col.is_unique ? 4 : 0) | (col.is_dropped ? 8 : 0);
        *buf = flags;
        buf++;
        // default_value
        u16 def_len = col.default_value.size();
        std::memcpy(buf, &def_len, 2);
        buf += 2;
        if (def_len > 0) {
            std::memcpy(buf, col.default_value.c_str(), def_len);
            buf += def_len;
        }
        // varchar_length (0 = unbounded / TEXT)
        std::memcpy(buf, &col.varchar_length, 4);
        buf += 4;
        // check_expr (length-prefixed)
        u16 chk_len = col.check_expr.size();
        std::memcpy(buf, &chk_len, 2);
        buf += 2;
        if (chk_len > 0) {
            std::memcpy(buf, col.check_expr.c_str(), chk_len);
            buf += chk_len;
        }
    }
    return buf;
}

Schema Schema::deserialize(const byte* buf) {
    return deserialize(buf, 1u << 20);
}

Schema Schema::deserialize(const byte* buf, u32 length) {
    Schema schema;
    if (!buf || length < 4) return schema;
    const byte* cur = buf;
    const byte* end = buf + length;
    u32 n;
    std::memcpy(&n, cur, 4);
    cur += 4;
    if (n > 4096) return schema;
    for (u32 i = 0; i < n; i++) {
        Column col;
        u16 name_len;
        if (cur + 2 > end) return Schema();
        std::memcpy(&name_len, cur, 2);
        cur += 2;
        if (cur + name_len > end) return Schema();
        col.name = String(reinterpret_cast<const char*>(cur), name_len);
        cur += name_len;
        // table_name
        u16 tbl_len;
        if (cur + 2 > end) return Schema();
        std::memcpy(&tbl_len, cur, 2);
        cur += 2;
        if (cur + tbl_len > end) return Schema();
        if (tbl_len > 0) {
            col.table_name = String(reinterpret_cast<const char*>(cur), tbl_len);
            cur += tbl_len;
        }
        if (cur + 2 > end) return Schema();
        col.type = static_cast<TypeId>(*cur);
        cur++;
        byte flags = *cur;
        cur++;
        col.not_null = (flags & 1) != 0;
        col.is_primary = (flags & 2) != 0;
        col.is_unique = (flags & 4) != 0;
        col.is_dropped = (flags & 8) != 0;
        // default_value
        if (cur + 2 > end) { schema.add_column(col); continue; }
        u16 def_len;
        std::memcpy(&def_len, cur, 2);
        cur += 2;
        if (def_len > 0 && cur + def_len <= end) {
            col.default_value = String(reinterpret_cast<const char*>(cur), def_len);
            cur += def_len;
        }
        // varchar_length is optional for forward compat — old serialised
        // schemas just stop here and the column defaults to 0 (unbounded).
        if (cur + 4 <= end) {
            std::memcpy(&col.varchar_length, cur, 4);
            cur += 4;
        }
        // check_expr (also optional; empty when absent)
        if (cur + 2 <= end) {
            u16 chk_len;
            std::memcpy(&chk_len, cur, 2);
            cur += 2;
            if (chk_len > 0 && cur + chk_len <= end) {
                col.check_expr = String(reinterpret_cast<const char*>(cur), chk_len);
                cur += chk_len;
            }
        }
        schema.add_column(col);
    }
    return schema;
}

u32 Schema::serialized_size() const {
    u32 size = 4;  // column count
    for (u32 i = 0; i < columns_.size(); i++) {
        size += 2 + columns_[i].name.size();                    // name_len + name
        size += 2 + columns_[i].table_name.size();              // table_name_len + table_name
        size += 1 + 1;                                          // type + flags
        size += 2 + columns_[i].default_value.size();           // default_value_len + default_value
        size += 4;                                              // varchar_length
        size += 2 + columns_[i].check_expr.size();              // check_expr_len + check_expr
    }
    return size;
}

} // namespace minidb
