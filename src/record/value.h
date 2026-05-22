/**
 * @file value.h
 * @brief Value type — Unified representation of all column values in the database
 *
 * Supports: BOOL, INT32, INT64, FLOAT, DOUBLE, VARCHAR, NULL
 * Serialization format: type_id(1B) + data (fixed/variable)
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include <cstring>

namespace minidb {

enum class TypeId : u8 {
    kBool    = 0,
    kInt32   = 1,
    kInt64   = 2,
    kFloat   = 3,
    kDouble  = 4,
    kVarchar = 5,
    kNull    = 255,
};

class Value {
public:
    // Constructor
    Value();                          // NULL
    explicit Value(bool val);
    explicit Value(i32 val);
    explicit Value(i64 val);
    explicit Value(float val);
    explicit Value(double val);
    explicit Value(const String& val);
    explicit Value(const char* val);

    // Copy / move.
    Value(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept;
    ~Value();

    // Type query.
    TypeId type_id() const { return type_id_; }
    bool is_null() const { return type_id_ == TypeId::kNull; }

    // Type-safe value accessors.
    bool    get_bool() const;
    i32     get_int32() const;
    i64     get_int64() const;
    float   get_float() const;
    double  get_double() const;
    const String& get_string() const;

    // Compare
    int compare(const Value& other) const;
    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const;
    bool operator<(const Value& other) const;
    bool operator>(const Value& other) const;
    bool operator<=(const Value& other) const;
    bool operator>=(const Value& other) const;

    // Arithmetic operators.
    Value operator+(const Value& other) const;
    Value operator-(const Value& other) const;
    Value operator*(const Value& other) const;
    Value operator/(const Value& other) const;
    Value operator%(const Value& other) const;

    // Serialize
    byte* serialize(byte* buf) const;
    static Value deserialize(const byte* buf, TypeId type);
    u32 serialized_size() const;

    // Type cast — returns NULL on failure instead of crashing.
    Value cast_to(TypeId target) const;

    // Convert to string.
    String to_string() const;

    // Type size.
    static u32 type_size(TypeId type);

private:
    void copy_from(const Value& other);
    void move_from(Value& other) noexcept;
    void destroy();

    TypeId type_id_;
    union {
        bool    bool_val_;
        i32     int32_val_;
        i64     int64_val_;
        float   float_val_;
        double  double_val_;
    };
    String string_val_;
};

} // namespace minidb
