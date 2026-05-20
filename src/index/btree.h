/**
 * @file btree.h
 * @brief B+ Tree index — supports multi-type key, correct parent tracking, page unpin
 *
 * Key slot layout (type-aware):
 *   [type_id u8][value up to 8B] = 9 bytes per key
 *
 * Leaf node:   [key0(9B) rid0(10B)] [key1(9B) rid1(10B)] ..., next_leaf at page end
 * Internal:    [key0(9B) child0(8B)] [key1(9B) child1(8B)] ...
 */
#pragma once

#include "common/defs.h"
#include "common/config.h"
#include "common/status.h"
#include "common/mutex.h"
#include "container/string.h"
#include "container/vector.h"
#include "record/value.h"
#include "storage/buffer_pool.h"

namespace minidb {

struct RecordId {
    PageId page_id;
    SlotIdx slot_idx;

    RecordId() : page_id(kNullPageId), slot_idx(0) {}
    RecordId(PageId pid, SlotIdx s) : page_id(pid), slot_idx(s) {}
    bool operator==(const RecordId& o) const {
        return page_id == o.page_id && slot_idx == o.slot_idx;
    }
    bool operator!=(const RecordId& o) const { return !(*this == o); }
};

static constexpr u32 kIndexNodeHeaderSize = 8;
static constexpr u32 kIndexKeyStart = kPageHeaderSize + kIndexNodeHeaderSize;
static constexpr u32 kRecordIdSize = 10;
static constexpr u32 kIndexKeyDataSize = 9;                         // type_id(1) + max value(8)
static constexpr u32 kIndexLeafSlotSize = kIndexKeyDataSize + kRecordIdSize;      // 19
static constexpr u32 kIndexInternalSlotSize = kIndexKeyDataSize + sizeof(PageId); // 17
static constexpr u32 kIndexMaxKeys = 400;

#pragma pack(push, 1)
struct BTreeMetaPage {
    PageId root_page_id;
    u32 next_page_num;
    u32 reserved;
};
#pragma pack(pop)

class BPlusTree : NonCopyable {
public:
    BPlusTree(u32 index_id, BufferPool* pool);
    ~BPlusTree() = default;

    void create();
    bool open();
    void insert(const Value& key, const RecordId& rid);
    bool remove(const Value& key, const RecordId& rid);
    Vector<RecordId> search(const Value& key);
    Vector<RecordId> range_search(const Value& low, const Value& high);
    bool scan_next(const Value& low, const Value& high,
                   PageId* leaf_id, u16* slot_idx,
                   const RecordId* skip_rid, RecordId* rid);
    bool scan_next_entry(const Value& low, const Value& high,
                         PageId* leaf_id, u16* slot_idx,
                         const RecordId* skip_rid, Value* key, RecordId* rid);
    bool validate_structure(String* error = nullptr) const;
    PageId root_page_id() const { return root_page_id_; }

private:
    bool validate_node(PageId page_id, u32 depth, u32* leaf_depth,
                       HashMap<PageId, bool>* visited, String* error) const;
    PageId find_leaf(const Value& key) const;
    PageId find_leaf_with_parent(const Value& key, PageId* parent_id) const;
    bool insert_into_leaf(PageId leaf_id, const Value& key, const RecordId& rid,
                          u16* num_keys_after);
    bool split_leaf(PageId leaf_id, PageId parent_id);
    bool insert_into_parent(PageId parent_id, const Value& key, PageId left, PageId right);
    bool split_internal(PageId node_id);
    PageId find_parent(PageId root, PageId child) const;

    // B+ tree removal helpers (M17)
    bool remove_from_leaf(PageId leaf_id, const Value& key, const RecordId& rid);
    void rebalance_after_remove(PageId leaf_id, PageId parent_id);
    void borrow_from_left_leaf(PageId leaf_id, PageId left_id, PageId parent_id, u16 parent_idx);
    void borrow_from_right_leaf(PageId leaf_id, PageId right_id, PageId parent_id, u16 parent_idx);
    void merge_leaves(PageId left_id, PageId right_id, PageId parent_id, u16 parent_idx);
    void borrow_from_left_internal(PageId node_id, PageId left_id, PageId parent_id, u16 parent_idx);
    void borrow_from_right_internal(PageId node_id, PageId right_id, PageId parent_id, u16 parent_idx);
    void merge_internal_nodes(PageId left_id, PageId right_id, PageId parent_id, u16 parent_idx);
    PageId alloc_page_id();
    PageId meta_page_id() const;
    void save_meta();
    void init_leaf_page(PageId page_id);
    PageId leftmost_leaf() const;
    void collect_all_entries(Vector<Pair<Value, RecordId>>& entries) const;
    void rebuild_from_entries(const Vector<Pair<Value, RecordId>>& entries);

    static Value read_key_from_slot(const byte* slot);
    static void write_key_to_slot(byte* slot, const Value& key);

    u16 leaf_num_keys(const Page* page) const;
    Value leaf_key(const Page* page, u16 idx) const;
    RecordId leaf_rid(const Page* page, u16 idx) const;
    PageId leaf_next(const Page* page) const;
    void leaf_set_keys(Page* page, u16 num_keys);
    void leaf_set_key(Page* page, u16 idx, const Value& key);
    void leaf_set_rid(Page* page, u16 idx, const RecordId& rid);
    void leaf_set_next(Page* page, PageId next);

    u16 internal_num_keys(const Page* page) const;
    Value internal_key(const Page* page, u16 idx) const;
    PageId internal_child(const Page* page, u16 idx) const;
    void internal_set_num_keys(Page* page, u16 n);
    void internal_set_key(Page* page, u16 idx, const Value& key);
    void internal_set_child(Page* page, u16 idx, PageId child);

    u32 index_id_;
    PageId root_page_id_;
    BufferPool* pool_;
    u32 next_page_num_;
    mutable RwLock tree_latch_;  // W8: Per-tree latch for concurrent safety
};

} // namespace minidb
