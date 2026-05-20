#include "index/btree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/page_store.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>
#include <unistd.h>

using namespace minidb;

struct Entry {
    i64 key;
    RecordId rid;
};

static String make_temp_dir(const char* pattern) {
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "%s/%s",
                  std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp",
                  pattern);
    char* path = mkdtemp(tmpl);
    assert(path != nullptr);
    return String(path);
}

static bool rid_less(const RecordId& a, const RecordId& b) {
    if (a.page_id != b.page_id) return a.page_id < b.page_id;
    return a.slot_idx < b.slot_idx;
}

static u64 rid_key(const RecordId& rid) {
    return (rid.page_id << 16) | static_cast<u64>(rid.slot_idx);
}

static bool entry_less(const Entry& a, const Entry& b) {
    if (a.key != b.key) return a.key < b.key;
    return rid_less(a.rid, b.rid);
}

static std::vector<RecordId> model_search(const std::multimap<i64, RecordId>& model, i64 key) {
    std::vector<RecordId> out;
    auto range = model.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        out.push_back(it->second);
    }
    return out;
}

static void assert_rids_equal(const Vector<RecordId>& actual, std::vector<RecordId> expected,
                              u64 seed, const char* context) {
    std::vector<u64> got;
    got.reserve(actual.size());
    for (u32 i = 0; i < actual.size(); i++) got.push_back(rid_key(actual[i]));
    std::vector<u64> want;
    want.reserve(expected.size());
    for (const RecordId& rid : expected) want.push_back(rid_key(rid));
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    if (got.size() != expected.size()) {
        std::fprintf(stderr, "btree_property_test FAIL seed=%llu context=%s size %zu expected %zu\n",
                     static_cast<unsigned long long>(seed), context, got.size(), want.size());
        std::abort();
    }
    for (size_t i = 0; i < got.size(); i++) {
        if (got[i] != want[i]) {
            std::fprintf(stderr, "btree_property_test FAIL seed=%llu context=%s mismatch at %zu\n",
                         static_cast<unsigned long long>(seed), context, i);
            std::abort();
        }
    }
}

static void validate_tree(BPlusTree& tree, const std::multimap<i64, RecordId>& model, u64 seed) {
    for (i64 key = -64; key <= 64; key++) {
        Vector<RecordId> actual = tree.search(Value(key));
        assert_rids_equal(actual, model_search(model, key), seed, "point-search");
    }

    std::vector<Entry> expected;
    expected.reserve(model.size());
    for (const auto& kv : model) expected.push_back(Entry{kv.first, kv.second});

    std::vector<Entry> actual_entries;
    PageId leaf = kNullPageId;
    u16 slot = 0;
    RecordId last;
    bool has_last = false;
    while (true) {
        Value key;
        RecordId rid;
        const RecordId* skip = has_last ? &last : nullptr;
        if (!tree.scan_next_entry(Value(static_cast<i64>(-128)), Value(static_cast<i64>(128)),
                                  &leaf, &slot, skip, &key, &rid)) {
            break;
        }
        actual_entries.push_back(Entry{key.get_int64(), rid});
        last = rid;
        has_last = true;
    }
    std::sort(actual_entries.begin(), actual_entries.end(), entry_less);

    if (actual_entries.size() != expected.size()) {
        std::fprintf(stderr, "btree_property_test FAIL seed=%llu full-scan size %zu expected %zu\n",
                     static_cast<unsigned long long>(seed),
                     actual_entries.size(), expected.size());
        std::abort();
    }
    for (size_t i = 0; i < expected.size(); i++) {
        if (actual_entries[i].key != expected[i].key ||
            !(actual_entries[i].rid == expected[i].rid)) {
            std::fprintf(stderr, "btree_property_test FAIL seed=%llu full-scan mismatch at %zu\n",
                         static_cast<unsigned long long>(seed), i);
            std::abort();
        }
    }
}

int main(int argc, char** argv) {
    u64 seed = 12648434;
    if (argc > 1) seed = static_cast<u64>(std::strtoull(argv[1], nullptr, 10));
    std::fprintf(stderr, "btree_property_test seed=%llu\n",
                 static_cast<unsigned long long>(seed));

    DiskManager disk(make_temp_dir("minidb-btree-prop.XXXXXX"), true, true, 64);
    LocalPageStore store(&disk);
    BufferPool pool(&store, 64, 1000, 64, 4, 16);
    BPlusTree tree(900, &pool);
    tree.create();

    std::mt19937_64 rng(seed);
    std::vector<Entry> entries;
    std::multimap<i64, RecordId> model;
    entries.reserve(2048);
    u32 next_rid = 1;

    for (u32 op = 0; op < 2500; op++) {
        bool do_insert = entries.empty() || (rng() % 100) < 65;
        if (do_insert) {
            i64 key = static_cast<i64>(rng() % 129) - 64;
            RecordId rid(make_page_id(901, next_rid++), static_cast<SlotIdx>(rng() % 128));
            tree.insert(Value(key), rid);
            entries.push_back(Entry{key, rid});
            model.insert(std::make_pair(key, rid));
        } else {
            size_t idx = static_cast<size_t>(rng() % entries.size());
            Entry victim = entries[idx];
            bool removed = tree.remove(Value(victim.key), victim.rid);
            if (!removed) {
                std::fprintf(stderr, "btree_property_test FAIL seed=%llu remove missed\n",
                             static_cast<unsigned long long>(seed));
                return 1;
            }
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(idx));
            auto range = model.equal_range(victim.key);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second == victim.rid) {
                    model.erase(it);
                    break;
                }
            }
        }

        if (op % 25 == 0) validate_tree(tree, model, seed);
    }

    validate_tree(tree, model, seed);
    pool.flush_all();
    return 0;
}
