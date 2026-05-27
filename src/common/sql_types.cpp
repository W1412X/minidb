#include "common/sql_types.h"

namespace minidb {

static String upper_ascii(String value) {
    for (u32 i = 0; i < value.size(); i++) {
        char& ch = value[i];
        if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
    }
    return value;
}

TypeId type_from_sql_name(const String& name) {
    String upper = upper_ascii(name);
    if (upper == "INT" || upper == "INTEGER") return TypeId::kInt32;
    if (upper == "BIGINT" || upper == "INT64") return TypeId::kInt64;
    if (upper == "FLOAT" || upper == "REAL") return TypeId::kFloat;
    if (upper == "DOUBLE" || upper == "DECIMAL" || upper == "NUMERIC") return TypeId::kDouble;
    if (upper == "VARCHAR" || upper == "TEXT") return TypeId::kVarchar;
    if (upper == "BOOL" || upper == "BOOLEAN") return TypeId::kBool;
    if (upper == "TIMESTAMP") return TypeId::kTimestamp;
    if (upper == "DATETIME") return TypeId::kDatetime;
    return TypeId::kVarchar;
}

const char* sql_name_from_type(TypeId type) {
    switch (type) {
        case TypeId::kBool: return "BOOL";
        case TypeId::kInt32: return "INT";
        case TypeId::kInt64: return "BIGINT";
        case TypeId::kFloat: return "FLOAT";
        case TypeId::kDouble: return "DOUBLE";
        case TypeId::kVarchar: return "TEXT";
        case TypeId::kTimestamp: return "TIMESTAMP";
        case TypeId::kDatetime: return "DATETIME";
        case TypeId::kNull: return "NULL";
        default: return "UNKNOWN";
    }
}

} // namespace minidb
