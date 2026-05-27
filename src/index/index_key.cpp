#include "index/index_key.h"
#include <cstring>

namespace minidb {

static constexpr u16 kIndexKeyMagic = 0x494b; // "IK"

void IndexKeySchema::add_column(TypeId type, bool descending, bool nulls_first,
                                u32 collation_id) {
    columns_.push_back(IndexKeyColumn(type, descending, nulls_first, collation_id));
}

IndexKey::IndexKey(const Value& value) {
    values_.push_back(value);
}

IndexKey::IndexKey(const Vector<Value>& values) : values_(values) {}

static int compare_value_with_null_order(const Value& a, const Value& b,
                                         bool nulls_first) {
    if (a.is_null() || b.is_null()) {
        if (a.is_null() && b.is_null()) return 0;
        if (a.is_null()) return nulls_first ? -1 : 1;
        return nulls_first ? 1 : -1;
    }
    return a.compare(b);
}

int IndexKey::compare(const IndexKey& other, const IndexKeySchema* schema) const {
    u32 n = values_.size() < other.values_.size() ? values_.size() : other.values_.size();
    for (u32 i = 0; i < n; i++) {
        bool desc = schema && i < schema->column_count() ? schema->column(i).descending : false;
        bool nulls_first = schema && i < schema->column_count() ? schema->column(i).nulls_first : true;
        int cmp = compare_value_with_null_order(values_[i], other.values_[i], nulls_first);
        if (cmp != 0) return desc ? -cmp : cmp;
    }
    if (values_.size() == other.values_.size()) return 0;
    return values_.size() < other.values_.size() ? -1 : 1;
}

bool IndexKey::starts_with(const IndexKey& prefix, const IndexKeySchema* schema) const {
    if (prefix.values_.size() > values_.size()) return false;
    for (u32 i = 0; i < prefix.values_.size(); i++) {
        bool desc = schema && i < schema->column_count() ? schema->column(i).descending : false;
        bool nulls_first = schema && i < schema->column_count() ? schema->column(i).nulls_first : true;
        int cmp = compare_value_with_null_order(values_[i], prefix.values_[i], nulls_first);
        if (desc) cmp = -cmp;
        if (cmp != 0) return false;
    }
    return true;
}

u32 IndexKey::encoded_size() const {
    u32 size = 2 + 2 + 2; // magic, count, payload size
    for (u32 i = 0; i < values_.size(); i++) {
        size += 1;
        switch (values_[i].type_id()) {
            case TypeId::kBool: size += 1; break;
            case TypeId::kInt32: size += 4; break;
            case TypeId::kInt64: size += 8; break;
            case TypeId::kTimestamp:
            case TypeId::kDatetime: size += 8; break;
            case TypeId::kFloat: size += 4; break;
            case TypeId::kDouble: size += 8; break;
            case TypeId::kVarchar: size += 2 + values_[i].get_string().size(); break;
            case TypeId::kNull: break;
        }
    }
    return size;
}

bool IndexKey::fits(u32 slot_size) const {
    return encoded_size() <= slot_size && values_.size() <= 255;
}

bool IndexKey::encode(byte* slot, u32 slot_size) const {
    if (!slot || !fits(slot_size)) return false;
    // Zero only the bytes we actually write + a small tail guard. Zeroing
    // the full slot_size (128+ bytes) on every encode was measurable overhead
    // on insert-heavy workloads. The decode path reads payload_size from the
    // header and stops there, so trailing bytes beyond encoded_size() are
    // never interpreted.
    u32 actual = encoded_size();
    std::memset(slot, 0, actual < slot_size ? actual + 1 : slot_size);
    byte* p = slot;
    u16 magic = kIndexKeyMagic;
    u16 count = static_cast<u16>(values_.size());
    u16 payload = static_cast<u16>(encoded_size() - 6);
    std::memcpy(p, &magic, 2); p += 2;
    std::memcpy(p, &count, 2); p += 2;
    std::memcpy(p, &payload, 2); p += 2;
    for (u32 i = 0; i < values_.size(); i++) {
        *p++ = static_cast<byte>(values_[i].type_id());
        switch (values_[i].type_id()) {
            case TypeId::kBool: *p++ = values_[i].get_bool() ? 1 : 0; break;
            case TypeId::kInt32: { i32 v = values_[i].get_int32(); std::memcpy(p, &v, 4); p += 4; break; }
            case TypeId::kInt64: { i64 v = values_[i].get_int64(); std::memcpy(p, &v, 8); p += 8; break; }
            case TypeId::kTimestamp:
            case TypeId::kDatetime: { i64 v = values_[i].get_datetime_micros(); std::memcpy(p, &v, 8); p += 8; break; }
            case TypeId::kFloat: { float v = values_[i].get_float(); std::memcpy(p, &v, 4); p += 4; break; }
            case TypeId::kDouble: { double v = values_[i].get_double(); std::memcpy(p, &v, 8); p += 8; break; }
            case TypeId::kVarchar: {
                const String& s = values_[i].get_string();
                u16 len = static_cast<u16>(s.size());
                std::memcpy(p, &len, 2); p += 2;
                if (len > 0) {
                    std::memcpy(p, s.c_str(), len);
                    p += len;
                }
                break;
            }
            case TypeId::kNull: break;
        }
    }
    return true;
}

IndexKey IndexKey::decode(const byte* slot, u32 slot_size) {
    if (!slot || slot_size < 6) return IndexKey();
    const byte* p = slot;
    const byte* end = slot + slot_size;
    u16 magic = 0;
    u16 count = 0;
    u16 payload = 0;
    std::memcpy(&magic, p, 2); p += 2;
    std::memcpy(&count, p, 2); p += 2;
    std::memcpy(&payload, p, 2); p += 2;
    if (magic != kIndexKeyMagic || payload + 6 > slot_size) return IndexKey();
    end = slot + 6 + payload;

    Vector<Value> values;
    for (u32 i = 0; i < count && p < end; i++) {
        TypeId type = static_cast<TypeId>(*p++);
        switch (type) {
            case TypeId::kBool:
                if (p + 1 > end) return IndexKey();
                values.push_back(Value(*p++ == 1));
                break;
            case TypeId::kInt32: {
                if (p + 4 > end) return IndexKey();
                i32 v; std::memcpy(&v, p, 4); p += 4; values.push_back(Value(v)); break;
            }
            case TypeId::kInt64: {
                if (p + 8 > end) return IndexKey();
                i64 v; std::memcpy(&v, p, 8); p += 8; values.push_back(Value(v)); break;
            }
            case TypeId::kTimestamp: {
                if (p + 8 > end) return IndexKey();
                i64 v; std::memcpy(&v, p, 8); p += 8; values.push_back(Value::timestamp(v)); break;
            }
            case TypeId::kDatetime: {
                if (p + 8 > end) return IndexKey();
                i64 v; std::memcpy(&v, p, 8); p += 8; values.push_back(Value::datetime(v)); break;
            }
            case TypeId::kFloat: {
                if (p + 4 > end) return IndexKey();
                float v; std::memcpy(&v, p, 4); p += 4; values.push_back(Value(v)); break;
            }
            case TypeId::kDouble: {
                if (p + 8 > end) return IndexKey();
                double v; std::memcpy(&v, p, 8); p += 8; values.push_back(Value(v)); break;
            }
            case TypeId::kVarchar: {
                if (p + 2 > end) return IndexKey();
                u16 len; std::memcpy(&len, p, 2); p += 2;
                if (p + len > end) return IndexKey();
                values.push_back(Value(String(reinterpret_cast<const char*>(p), len)));
                p += len;
                break;
            }
            case TypeId::kNull:
            default:
                values.push_back(Value());
                break;
        }
    }
    return IndexKey(values);
}

String IndexKey::to_string() const {
    if (values_.size() == 1) return values_[0].to_string();
    String out("(");
    for (u32 i = 0; i < values_.size(); i++) {
        if (i > 0) out += ",";
        out += values_[i].to_string();
    }
    out += ")";
    return out;
}

} // namespace minidb
