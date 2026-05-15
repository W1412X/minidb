#include "record/tuple.h"
#include "record/value.h"
#include "container/hash_map.h"
#include "storage/page.h"
#include <cassert>
#include <cstring>

using namespace minidb;

static Schema make_schema() {
    Schema schema;
    schema.add_column(Column("a", TypeId::kInt32));
    schema.add_column(Column("b", TypeId::kVarchar));
    return schema;
}

static void assert_hash_map_tombstones_preserve_probe_chain() {
    HashMap<u32, u32> map(16);
    Hash<u32> hash;
    u32 keys[3] = {0, 0, 0};
    u32 found = 0;
    u32 bucket = hash(1) % 16;
    for (u32 candidate = 1; candidate < 4096 && found < 3; candidate++) {
        if (hash(candidate) % 16 == bucket) {
            keys[found++] = candidate;
        }
    }
    assert(found == 3);

    map[keys[0]] = 10;
    map[keys[1]] = 20;
    map[keys[2]] = 30;
    assert(map.erase(keys[0]));
    assert(map.find(keys[1]) && *map.find(keys[1]) == 20);
    assert(map.find(keys[2]) && *map.find(keys[2]) == 30);
    assert(map.erase(keys[1]));
    map[keys[0]] = 40;
    assert(map.find(keys[0]) && *map.find(keys[0]) == 40);
    assert(map.find(keys[2]) && *map.find(keys[2]) == 30);

    map.clear();
    map[keys[2]] = 50;
    assert(map.size() == 1);
    assert(map.find(keys[2]) && *map.find(keys[2]) == 50);
}

int main() {
    assert_hash_map_tombstones_preserve_probe_chain();

    Value bool_val(true);
    Value int_val(static_cast<i32>(2));
    Value sum = bool_val + int_val;
    assert(!sum.is_null());
    assert(sum.type_id() == TypeId::kInt32);
    assert(sum.get_int32() == 3);

    Value text("x");
    assert((text + int_val).is_null());

    Schema schema = make_schema();
    byte corrupt[kPageSize];
    std::memset(corrupt, 0, sizeof(corrupt));
    u64 xmin = 1;
    std::memcpy(corrupt, &xmin, 8);
    u32 huge_cols = 0xFFFFFFFFu;
    std::memcpy(corrupt + 26, &huge_cols, 4);
    Tuple bad = Tuple::deserialize_from_page(corrupt, schema, sizeof(corrupt));
    assert(bad.column_count() == 0);

    Vector<Value> values;
    values.push_back(Value(static_cast<i32>(42)));
    Tuple old_tuple(schema, values);
    byte tuple_buf[kPageSize];
    byte* end = old_tuple.serialize_to_page(tuple_buf);
    Tuple padded = Tuple::deserialize_from_page(tuple_buf, schema,
                                                static_cast<u32>(end - tuple_buf));
    assert(padded.column_count() == 2);
    assert(padded.get_value(0).get_int32() == 42);
    assert(padded.get_value(1).is_null());

    byte schema_buf[16];
    std::memset(schema_buf, 0, sizeof(schema_buf));
    u32 one_col = 1;
    u16 impossible_name_len = 1024;
    std::memcpy(schema_buf, &one_col, 4);
    std::memcpy(schema_buf + 4, &impossible_name_len, 2);
    Schema corrupt_schema = Schema::deserialize(schema_buf, sizeof(schema_buf));
    assert(corrupt_schema.column_count() == 0);

    byte schema_roundtrip[256];
    byte* schema_end = schema.serialize(schema_roundtrip);
    Schema decoded = Schema::deserialize(
        schema_roundtrip, static_cast<u32>(schema_end - schema_roundtrip));
    assert(decoded.column_count() == schema.column_count());
    assert(decoded.get_column(0).name == schema.get_column(0).name);
    assert(decoded.get_column(1).type == TypeId::kVarchar);

    Schema physical_schema = make_schema();
    physical_schema.add_column(Column("legacy_tail", TypeId::kInt32));
    Vector<Value> physical_values;
    physical_values.push_back(Value(static_cast<i32>(7)));
    physical_values.push_back(Value("kept"));
    physical_values.push_back(Value(static_cast<i32>(999)));
    Tuple physical_tuple(physical_schema, physical_values);
    byte physical_buf[kPageSize];
    byte* physical_end = physical_tuple.serialize_to_page(physical_buf);
    Tuple logical_tuple = Tuple::deserialize_from_page(
        physical_buf, schema, static_cast<u32>(physical_end - physical_buf));
    assert(logical_tuple.column_count() == 2);
    assert(logical_tuple.get_value(0).get_int32() == 7);
    assert(logical_tuple.get_value(1).get_string() == "kept");

    Page page;
    page.init(make_page_id(10, 1), PageType::kHeapData);
    byte payload[200];
    std::memset(payload, 7, sizeof(payload));
    for (u32 i = 0; i < 30; i++) {
        if (page.insert_tuple(payload, sizeof(payload)) == kNullSlot) break;
    }
    assert(page.reclaim_slot(0));
    assert(page.mark_dead(1));
    assert(page.has_enough_space(160));

    return 0;
}
