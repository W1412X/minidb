#include "index/btree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/page_store.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace minidb;

static void check(bool cond, const char* expr, const char* file, int line) {
    if (cond) return;
    std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", file, line, expr);
    std::fflush(stderr);
    std::abort();
}

#undef assert
#define assert(expr) check((expr), #expr, __FILE__, __LINE__)

static String make_temp_dir(const char* pattern) {
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "%s/%s",
                  std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp",
                  pattern);
    char* path = mkdtemp(tmpl);
    assert(path != nullptr);
    return String(path);
}

static IndexKey composite(i64 a, const char* b) {
    Vector<Value> values;
    values.push_back(Value(a));
    values.push_back(Value(String(b)));
    return IndexKey::from_values(values);
}

static void assert_contains(const Vector<RecordId>& rids, const RecordId& rid) {
    for (u32 i = 0; i < rids.size(); i++) {
        if (rids[i] == rid) return;
    }
    std::fprintf(stderr, "index_key_btree_test missing rid page=%llu slot=%u\n",
                 static_cast<unsigned long long>(rid.page_id), rid.slot_idx);
    std::abort();
}

int main() {
    DiskManager disk(make_temp_dir("minidb-index-key.XXXXXX"), true, true, 64);
    LocalPageStore store(&disk);
    BufferPool pool(&store, 64, 1000, 64, 4, 16);

    BPlusTree composite_tree(910, &pool);
    composite_tree.create();
    RecordId r1(make_page_id(911, 1), 1);
    RecordId r2(make_page_id(911, 2), 2);
    RecordId r3(make_page_id(911, 3), 3);
    RecordId r4(make_page_id(911, 4), 4);
    assert(composite_tree.insert(composite(1, "aa"), r1));
    assert(composite_tree.insert(composite(1, "bb"), r2));
    assert(composite_tree.insert(composite(2, "aa"), r3));
    assert(composite_tree.insert(composite(1, "cc"), r4));

    Vector<RecordId> exact = composite_tree.search(composite(1, "bb"));
    assert(exact.size() == 1);
    assert(exact[0] == r2);

    Vector<Value> prefix_values;
    prefix_values.push_back(Value(static_cast<i64>(1)));
    assert(composite(1, "aa").starts_with(IndexKey::from_values(prefix_values)));
    Vector<RecordId> prefix = composite_tree.search(IndexKey::from_values(prefix_values));
    assert(prefix.size() == 3);
    assert_contains(prefix, r1);
    assert_contains(prefix, r2);
    assert_contains(prefix, r4);

    assert(composite_tree.remove(composite(1, "bb"), r2));
    assert(composite_tree.search(composite(1, "bb")).empty());
    String err;
    assert(composite_tree.validate_structure(&err));

    BPlusTree text_tree(912, &pool);
    text_tree.create();
    assert(text_tree.insert(IndexKey::single(Value(String("alice"))),
                            RecordId(make_page_id(913, 1), 1)));
    assert(text_tree.insert(IndexKey::single(Value(String("bob"))),
                            RecordId(make_page_id(913, 2), 2)));
    assert(text_tree.insert(IndexKey::single(Value(String("carol"))),
                            RecordId(make_page_id(913, 3), 3)));

    Vector<RecordId> bob = text_tree.search(IndexKey::single(Value(String("bob"))));
    assert(bob.size() == 1);
    assert(bob[0].slot_idx == 2);

    Vector<RecordId> text_range = text_tree.range_search(
        IndexKey::single(Value(String("b"))),
        IndexKey::single(Value(String("czz"))));
    assert(text_range.size() == 2);

    String large(600, 'x');
    assert(!IndexKey::single(Value(large)).fits());
    assert(!text_tree.insert(IndexKey::single(Value(large)),
                             RecordId(make_page_id(913, 4), 4)));

    pool.flush_all();
    return 0;
}
