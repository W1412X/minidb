/**
 * @file value.cpp
 * @brief Value implementation
 */
#include "record/value.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <limits>

namespace minidb {

// ============================================================
// Constructor / Destructor
// ============================================================

Value::Value() : type_id_(TypeId::kNull), bool_val_(false) {}

Value::Value(bool val) : type_id_(TypeId::kBool), bool_val_(val) {}

Value::Value(i32 val) : type_id_(TypeId::kInt32), int32_val_(val) {}

Value::Value(i64 val) : type_id_(TypeId::kInt64), int64_val_(val) {}

Value::Value(float val) : type_id_(TypeId::kFloat), float_val_(val) {}

Value::Value(double val) : type_id_(TypeId::kDouble), double_val_(val) {}

Value::Value(const String& val) : type_id_(TypeId::kVarchar), string_val_(val) {}

Value::Value(const char* val) : type_id_(TypeId::kVarchar), string_val_(val) {}

Value::Value(const Value& other) : type_id_(TypeId::kNull), bool_val_(false) {
    copy_from(other);
}

Value::Value(Value&& other) noexcept : type_id_(TypeId::kNull), bool_val_(false) {
    move_from(other);
}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        destroy();
        copy_from(other);
    }
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        destroy();
        move_from(other);
    }
    return *this;
}

Value::~Value() {
    destroy();
}

// ============================================================
// Get value
// ============================================================

bool Value::get_bool() const { return bool_val_; }
i32  Value::get_int32() const { return int32_val_; }
i64  Value::get_int64() const { return int64_val_; }
float Value::get_float() const { return float_val_; }
double Value::get_double() const { return double_val_; }
const String& Value::get_string() const { return string_val_; }

// ============================================================
// Compare
// ============================================================

int Value::compare(const Value& other) const {
    if (type_id_ != other.type_id_) {
        // When Types differ, sort by Type ID
        return static_cast<int>(type_id_) - static_cast<int>(other.type_id_);
    }

    switch (type_id_) {
        case TypeId::kNull:    return 0;
        case TypeId::kBool:    return (bool_val_ < other.bool_val_) ? -1 : (bool_val_ > other.bool_val_) ? 1 : 0;
        case TypeId::kInt32:   return (int32_val_ < other.int32_val_) ? -1 : (int32_val_ > other.int32_val_) ? 1 : 0;
        case TypeId::kInt64:   return (int64_val_ < other.int64_val_) ? -1 : (int64_val_ > other.int64_val_) ? 1 : 0;
        case TypeId::kFloat: {
            bool lhs_nan = (float_val_ != float_val_);
            bool rhs_nan = (other.float_val_ != other.float_val_);
            if (lhs_nan || rhs_nan) return lhs_nan ? (rhs_nan ? 0 : 1) : -1;
            return (float_val_ < other.float_val_) ? -1 : (float_val_ > other.float_val_) ? 1 : 0;
        }
        case TypeId::kDouble: {
            bool lhs_nan = (double_val_ != double_val_);
            bool rhs_nan = (other.double_val_ != other.double_val_);
            if (lhs_nan || rhs_nan) return lhs_nan ? (rhs_nan ? 0 : 1) : -1;
            return (double_val_ < other.double_val_) ? -1 : (double_val_ > other.double_val_) ? 1 : 0;
        }
        case TypeId::kVarchar: return string_val_.compare(other.string_val_);
        default: return 0;
    }
}

bool Value::operator==(const Value& o) const { return compare(o) == 0; }
bool Value::operator!=(const Value& o) const { return compare(o) != 0; }
bool Value::operator<(const Value& o) const  { return compare(o) < 0; }
bool Value::operator>(const Value& o) const  { return compare(o) > 0; }
bool Value::operator<=(const Value& o) const { return compare(o) <= 0; }
bool Value::operator>=(const Value& o) const { return compare(o) >= 0; }

// ============================================================
// Arithmetic — preserve type (PostgreSQL rules)
// ============================================================

// Type promotion: two different types, promote to wider type
static TypeId promote(TypeId a, TypeId b) {
    // kBool is promoted to kInt32 in arithmetic
    if (a == TypeId::kBool) a = TypeId::kInt32;
    if (b == TypeId::kBool) b = TypeId::kInt32;
    if (a == b) return a;
    if (a == TypeId::kDouble || b == TypeId::kDouble) return TypeId::kDouble;
    if (a == TypeId::kFloat || b == TypeId::kFloat) return TypeId::kFloat;
    if (a == TypeId::kInt64 || b == TypeId::kInt64) return TypeId::kInt64;
    return TypeId::kInt32;
}

// Type-safe accessor: reads the correct union member based on the actual type_id.
static i64 to_i64(const Value& v) {
    switch (v.type_id()) {
        case TypeId::kBool:   return v.get_bool() ? 1 : 0;
        case TypeId::kInt32:  return v.get_int32();
        case TypeId::kInt64:  return v.get_int64();
        default: return 0;
    }
}

static double to_double_val(const Value& v) {
    switch (v.type_id()) {
        case TypeId::kBool:   return v.get_bool() ? 1.0 : 0.0;
        case TypeId::kInt32:  return v.get_int32();
        case TypeId::kInt64:  return v.get_int64();
        case TypeId::kFloat:  return v.get_float();
        case TypeId::kDouble: return v.get_double();
        default: return 0.0;
    }
}

static float to_float_val(const Value& v) {
    switch (v.type_id()) {
        case TypeId::kBool:   return v.get_bool() ? 1.0f : 0.0f;
        case TypeId::kInt32:  return static_cast<float>(v.get_int32());
        case TypeId::kInt64:  return static_cast<float>(v.get_int64());
        case TypeId::kFloat:  return v.get_float();
        default: return 0.0f;
    }
}

Value Value::operator+(const Value& o) const {
    if (is_null() || o.is_null()) return Value();
    if (type_id_ == TypeId::kVarchar || o.type_id_ == TypeId::kVarchar) {
        if (type_id_ != TypeId::kVarchar || o.type_id_ != TypeId::kVarchar) return Value();
        return Value(string_val_ + o.string_val_);
    }
    TypeId result_type = promote(type_id_, o.type_id_);
    switch (result_type) {
        case TypeId::kInt32: {
            i64 lhs = to_i64(*this);
            i64 rhs = to_i64(o);
            i64 result = lhs + rhs;
            if (result < std::numeric_limits<i32>::min() ||
                result > std::numeric_limits<i32>::max()) {
                return Value();
            }
            return Value(static_cast<i32>(result));
        }
        case TypeId::kInt64: {
            i64 result;
            if (__builtin_add_overflow(to_i64(*this), to_i64(o), &result)) return Value();
            return Value(result);
        }
        case TypeId::kFloat:  return Value(to_float_val(*this) + to_float_val(o));
        case TypeId::kDouble: return Value(to_double_val(*this) + to_double_val(o));
        default: return Value();
    }
}

Value Value::operator-(const Value& o) const {
    if (is_null() || o.is_null()) return Value();
    TypeId result_type = promote(type_id_, o.type_id_);
    switch (result_type) {
        case TypeId::kInt32: {
            i64 lhs = to_i64(*this);
            i64 rhs = to_i64(o);
            i64 result = lhs - rhs;
            if (result < std::numeric_limits<i32>::min() ||
                result > std::numeric_limits<i32>::max()) {
                return Value();
            }
            return Value(static_cast<i32>(result));
        }
        case TypeId::kInt64: {
            i64 result;
            if (__builtin_sub_overflow(to_i64(*this), to_i64(o), &result)) return Value();
            return Value(result);
        }
        case TypeId::kFloat:  return Value(to_float_val(*this) - to_float_val(o));
        case TypeId::kDouble: return Value(to_double_val(*this) - to_double_val(o));
        default: return Value();
    }
}

Value Value::operator*(const Value& o) const {
    if (is_null() || o.is_null()) return Value();
    TypeId result_type = promote(type_id_, o.type_id_);
    switch (result_type) {
        case TypeId::kInt32: {
            i64 lhs = to_i64(*this);
            i64 rhs = to_i64(o);
            i64 result = lhs * rhs;
            if (result < std::numeric_limits<i32>::min() ||
                result > std::numeric_limits<i32>::max()) {
                return Value();
            }
            return Value(static_cast<i32>(result));
        }
        case TypeId::kInt64: {
            i64 result;
            if (__builtin_mul_overflow(to_i64(*this), to_i64(o), &result)) return Value();
            return Value(result);
        }
        case TypeId::kFloat:  return Value(to_float_val(*this) * to_float_val(o));
        case TypeId::kDouble: return Value(to_double_val(*this) * to_double_val(o));
        default: return Value();
    }
}

Value Value::operator/(const Value& o) const {
    if (is_null() || o.is_null()) return Value();
    TypeId result_type = promote(type_id_, o.type_id_);
    switch (result_type) {
        case TypeId::kInt32:  {
            i64 lhs = to_i64(*this);
            i64 rhs = to_i64(o);
            if (rhs == 0) return Value();
            if (lhs == std::numeric_limits<i32>::min() && rhs == -1) return Value();
            return Value(static_cast<i32>(lhs / rhs));
        }
        case TypeId::kInt64:  {
            i64 lhs = to_i64(*this);
            i64 rhs = to_i64(o);
            if (rhs == 0) return Value();
            if (lhs == std::numeric_limits<i64>::min() && rhs == -1) return Value();
            return Value(lhs / rhs);
        }
        case TypeId::kFloat:  { return Value(to_float_val(*this) / to_float_val(o)); }
        case TypeId::kDouble: { return Value(to_double_val(*this) / to_double_val(o)); }
        default: return Value();
    }
}

Value Value::operator%(const Value& o) const {
    if (is_null() || o.is_null()) return Value();
    if (type_id_ == TypeId::kInt32 && o.type_id_ == TypeId::kInt32) {
        if (o.int32_val_ == 0) return Value();
        return Value(int32_val_ % o.int32_val_);
    }
    if (type_id_ == TypeId::kInt64 && o.type_id_ == TypeId::kInt64) {
        if (o.int64_val_ == 0) return Value();
        return Value(int64_val_ % o.int64_val_);
    }
    return Value();
}

// ============================================================
// Serialize
// ============================================================

byte* Value::serialize(byte* buf) const {
    *buf = static_cast<byte>(type_id_);
    buf++;
    switch (type_id_) {
        case TypeId::kBool:
            *buf = bool_val_ ? 1 : 0;
            buf++;
            break;
        case TypeId::kInt32:
            std::memcpy(buf, &int32_val_, 4);
            buf += 4;
            break;
        case TypeId::kInt64:
            std::memcpy(buf, &int64_val_, 8);
            buf += 8;
            break;
        case TypeId::kFloat:
            std::memcpy(buf, &float_val_, 4);
            buf += 4;
            break;
        case TypeId::kDouble:
            std::memcpy(buf, &double_val_, 8);
            buf += 8;
            break;
        case TypeId::kVarchar: {
            u32 len = string_val_.size();
            std::memcpy(buf, &len, 4);
            buf += 4;
            if (len > 0) {
                std::memcpy(buf, string_val_.c_str(), len);
                buf += len;
            }
            break;
        }
        case TypeId::kNull:
            break;
    }
    return buf;
}

Value Value::deserialize(const byte* buf, TypeId /*schema_type*/) {
    // Read the actual type_id from the serialized data, do not trust the schema.
    TypeId type = static_cast<TypeId>(*buf);
    buf++;  // skip type_id
    switch (type) {
        case TypeId::kBool:    return Value(*buf == 1);
        case TypeId::kInt32: { i32 v; std::memcpy(&v, buf, 4); return Value(v); }
        case TypeId::kInt64: { i64 v; std::memcpy(&v, buf, 8); return Value(v); }
        case TypeId::kFloat: { float v; std::memcpy(&v, buf, 4); return Value(v); }
        case TypeId::kDouble: { double v; std::memcpy(&v, buf, 8); return Value(v); }
        case TypeId::kVarchar: {
            u32 len;
            std::memcpy(&len, buf, 4);
            buf += 4;
            return Value(String(reinterpret_cast<const char*>(buf), len));
        }
        case TypeId::kNull:
        default:
            return Value();
    }
}

u32 Value::serialized_size() const {
    switch (type_id_) {
        case TypeId::kBool:    return 2;   // type_id + bool
        case TypeId::kInt32:   return 5;   // type_id + i32
        case TypeId::kInt64:   return 9;   // type_id + i64
        case TypeId::kFloat:   return 5;   // type_id + float
        case TypeId::kDouble:  return 9;   // type_id + double
        case TypeId::kVarchar: return 1 + 4 + string_val_.size();
        case TypeId::kNull:    return 1;
        default: return 1;
    }
}

// ============================================================
// Type cast — returns NULL on failure.
// ============================================================

Value Value::cast_to(TypeId target) const {
    if (type_id_ == TypeId::kNull) return Value();
    if (type_id_ == target) return *this;

    // ── Fast path: numeric-to-numeric without string round-trip ──
    switch (target) {
        case TypeId::kInt32: {
            switch (type_id_) {
                case TypeId::kInt64:  return Value(static_cast<i32>(int64_val_));
                case TypeId::kFloat:  return Value(static_cast<i32>(float_val_));
                case TypeId::kDouble: return Value(static_cast<i32>(double_val_));
                case TypeId::kBool:   return Value(static_cast<i32>(bool_val_ ? 1 : 0));
                default: break;
            }
            break;
        }
        case TypeId::kInt64: {
            switch (type_id_) {
                case TypeId::kInt32:  return Value(static_cast<i64>(int32_val_));
                case TypeId::kFloat:  return Value(static_cast<i64>(float_val_));
                case TypeId::kDouble: return Value(static_cast<i64>(double_val_));
                case TypeId::kBool:   return Value(static_cast<i64>(bool_val_ ? 1 : 0));
                default: break;
            }
            break;
        }
        case TypeId::kFloat: {
            switch (type_id_) {
                case TypeId::kInt32:  return Value(static_cast<float>(int32_val_));
                case TypeId::kInt64:  return Value(static_cast<float>(int64_val_));
                case TypeId::kDouble: return Value(static_cast<float>(double_val_));
                case TypeId::kBool:   return Value(static_cast<float>(bool_val_ ? 1.0f : 0.0f));
                default: break;
            }
            break;
        }
        case TypeId::kDouble: {
            switch (type_id_) {
                case TypeId::kInt32:  return Value(static_cast<double>(int32_val_));
                case TypeId::kInt64:  return Value(static_cast<double>(int64_val_));
                case TypeId::kFloat:  return Value(static_cast<double>(float_val_));
                case TypeId::kBool:   return Value(static_cast<double>(bool_val_ ? 1.0 : 0.0));
                default: break;
            }
            break;
        }
        case TypeId::kBool: {
            switch (type_id_) {
                case TypeId::kInt32:  return Value(int32_val_ != 0);
                case TypeId::kInt64:  return Value(int64_val_ != 0);
                case TypeId::kFloat:  return Value(float_val_ != 0.0f);
                case TypeId::kDouble: return Value(double_val_ != 0.0);
                default: break;
            }
            break;
        }
        default:
            break;
    }

    // ── Slow path: via string representation (varchar source or target) ──
    String s = to_string();

    switch (target) {
        case TypeId::kInt32: {
            char* end = nullptr;
            long v = strtol(s.c_str(), &end, 10);
            if (end == s.c_str() || *end != '\0') return Value();
            if (v < INT32_MIN || v > INT32_MAX) return Value();
            return Value(static_cast<i32>(v));
        }
        case TypeId::kInt64: {
            char* end = nullptr;
            long long v = strtoll(s.c_str(), &end, 10);
            if (end == s.c_str() || *end != '\0') return Value();
            return Value(static_cast<i64>(v));
        }
        case TypeId::kFloat: {
            char* end = nullptr;
            float v = strtof(s.c_str(), &end);
            if (end == s.c_str()) return Value();
            return Value(v);
        }
        case TypeId::kDouble: {
            char* end = nullptr;
            double v = strtod(s.c_str(), &end);
            if (end == s.c_str()) return Value();
            return Value(v);
        }
        case TypeId::kBool: {
            if (s == "true" || s == "1" || s == "t" || s == "TRUE") return Value(true);
            if (s == "false" || s == "0" || s == "f" || s == "FALSE") return Value(false);
            return Value();
        }
        case TypeId::kVarchar:
            return Value(s);
        default:
            return Value();
    }
}

// ============================================================
// Convert to string.
// ============================================================

String Value::to_string() const {
    char buf[64];
    int len = 0;
    switch (type_id_) {
        case TypeId::kBool:    return bool_val_ ? "1" : "0";
        case TypeId::kInt32:   len = snprintf(buf, sizeof(buf), "%d", int32_val_); break;
        case TypeId::kInt64:   len = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(int64_val_)); break;
        case TypeId::kFloat:   len = snprintf(buf, sizeof(buf), "%f", float_val_); break;
        case TypeId::kDouble:  len = snprintf(buf, sizeof(buf), "%f", double_val_); break;
        case TypeId::kVarchar: return string_val_;
        case TypeId::kNull:    return "NULL";
        default: return "?";
    }
    return String(buf, static_cast<String::size_type>(len));
}

u32 Value::type_size(TypeId type) {
    switch (type) {
        case TypeId::kBool:    return 1;
        case TypeId::kInt32:   return 4;
        case TypeId::kInt64:   return 8;
        case TypeId::kFloat:   return 4;
        case TypeId::kDouble:  return 8;
        case TypeId::kVarchar: return 0;  // variable-length
        case TypeId::kNull:    return 0;
        default: return 0;
    }
}

// ============================================================
// Internal helpers.
// ============================================================

void Value::copy_from(const Value& other) {
    type_id_ = other.type_id_;
    switch (type_id_) {
        case TypeId::kBool:    bool_val_ = other.bool_val_; break;
        case TypeId::kInt32:   int32_val_ = other.int32_val_; break;
        case TypeId::kInt64:   int64_val_ = other.int64_val_; break;
        case TypeId::kFloat:   float_val_ = other.float_val_; break;
        case TypeId::kDouble:  double_val_ = other.double_val_; break;
        case TypeId::kVarchar: string_val_ = other.string_val_; break;
        case TypeId::kNull:
        default: break;
    }
}

void Value::move_from(Value& other) noexcept {
    type_id_ = other.type_id_;
    switch (type_id_) {
        case TypeId::kBool:    bool_val_ = other.bool_val_; break;
        case TypeId::kInt32:   int32_val_ = other.int32_val_; break;
        case TypeId::kInt64:   int64_val_ = other.int64_val_; break;
        case TypeId::kFloat:   float_val_ = other.float_val_; break;
        case TypeId::kDouble:  double_val_ = other.double_val_; break;
        case TypeId::kVarchar: string_val_ = static_cast<String&&>(other.string_val_); break;
        case TypeId::kNull:
        default: break;
    }
    other.type_id_ = TypeId::kNull;
}

void Value::destroy() {
    // String's destructor handles cleanup.
    type_id_ = TypeId::kNull;
}

} // namespace minidb
