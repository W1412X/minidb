/**
 * @file btree.h
 * @brief B+ Tree index — supports multi-type key, correct parent tracking, page unpin
 *
 * Key slot layout:
 *   fixed-size serialized IndexKey supporting composite and varlen values
 *
 * Leaf node:   [serialized key0 rid0] [serialized key1 rid1] ..., next_leaf at page end
 * Internal:    [serialized key0 child0] [serialized key1 child1] ...
 */
#pragma once

#include "common/defs.h"
#include "common/config.h"
#include "common/status.h"
#include "common/mutex.h"
#include "container/string.h"
#include "container/vector.h"
#include "index/index_key.h"
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
static constexpr u32 kIndexKeyDataSize = kIndexKeyMaxEncodedSize;
static constexpr u32 kIndexLeafSlotSize = kIndexKeyDataSize + kRecordIdSize;
static constexpr u32 kIndexInternalSlotSize = kIndexKeyDataSize + sizeof(PageId);
static constexpr u32 kIndexLeafPhysicalMaxKeys =
    (kPageSize - kIndexKeyStart - sizeof(PageId)) / kIndexLeafSlotSize;
static constexpr u32 kIndexInternalPhysicalMaxKeys =
    (kPageSize - kIndexKeyStart - sizeof(PageId) - kIndexKeyDataSize - sizeof(PageId)) /
    kIndexInternalSlotSize;
// Keep one physical slot as a temporary overflow slot. Insert paths write the
// new entry first and then split when num_keys > kIndexMaxKeys. Internal
// nodes need room for the extra rightmost child pointer at child index n.
static constexpr u32 kIndexMaxKeys =
    ((kIndexLeafPhysicalMaxKeys < kIndexInternalPhysicalMaxKeys)
         ? kIndexLeafPhysicalMaxKeys
         : kIndexInternalPhysicalMaxKeys) > 1
        ? ((kIndexLeafPhysicalMaxKeys < kIndexInternalPhysicalMaxKeys)
               ? kIndexLeafPhysicalMaxKeys
               : kIndexInternalPhysicalMaxKeys) - 1
        : 1;

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
    bool insert(const IndexKey& key, const RecordId& rid);
    bool remove(const IndexKey& key, const RecordId& rid);
    Vector<RecordId> search(const IndexKey& key);
    Vector<RecordId> range_search(const IndexKey& low, const IndexKey& high);
    bool scan_next(const IndexKey& low, const IndexKey& high,
                   PageId* leaf_id, u16* slot_idx,
                   const RecordId* skip_rid, RecordId* rid);
    bool scan_next_entry(const IndexKey& low, const IndexKey& high,
                         PageId* leaf_id, u16* slot_idx,
                         const RecordId* skip_rid, IndexKey* key, RecordId* rid);

    // Batched range iterator: returns up to `capacity` (key, RecordId) pairs
    // matching [low, high] in a single call. The leaf page is fetched and
    // released exactly once per call regardless of how many entries it
    // yields, which collapses N×(per-row latch + hash-map lookup + atomic
    // pin) into a single set per batch. Cursor state lives in the caller-
    // owned PageId/slot pair, identical to scan_next_entry's contract.
    //
    // Returns the number of entries written. A return < capacity means
    // either the range is exhausted (leaf_id reset to kNullPageId) or this
    // leaf yielded everything in range; the caller can keep calling until
    // the returned count is 0 to fully drain the iterator.
    u32 range_scan_batch(const IndexKey& low, const IndexKey& high,
                         PageId* leaf_id, u16* slot_idx,
                         const RecordId* skip_rid,
                         IndexKey* out_keys, RecordId* out_rids,
                         u32 capacity);
    bool validate_structure(String* error = nullptr) const;
    PageId root_page_id() const { return root_page_id_; }

private:
    bool validate_node(PageId page_id, u32 depth, u32* leaf_depth,
                       HashMap<PageId, bool>* visited, String* error) const;
    PageId find_leaf(const IndexKey& key) const;
    PageId find_leaf_with_parent(const IndexKey& key, PageId* parent_id) const;
    bool insert_into_leaf(PageId leaf_id, const IndexKey& key, const RecordId& rid,
                          u16* num_keys_after);
    bool split_leaf(PageId leaf_id, PageId parent_id);
    bool insert_into_parent(PageId parent_id, const IndexKey& key, PageId left, PageId right);
    bool split_internal(PageId node_id);
    PageId find_parent(PageId root, PageId child) const;

    // B+ tree removal helpers (M17)
    bool remove_from_leaf(PageId leaf_id, const IndexKey& key, const RecordId& rid);
    void rebalance_after_remove(PageId leaf_id, PageId parent_id);
    void borrow_from_left_leaf(PageId leaf_id, PageId left_id, PageId parent_id, u16 parent_idx);
    void borrow_from_right_leaf(PageId leaf_id, PageId right_id, PageId parent_id, u16 parent_idx);
    void merge_leaves(PageId left_id, PageId right_id, PageId parent_id, u16 parent_idx);
    void unlink_empty_leaf(PageId leaf_id);
    bool first_key_in_subtree(PageId page_id, IndexKey* key) const;
    void refresh_internal_separators(PageId page_id);
    void borrow_from_left_internal(PageId node_id, PageId left_id, PageId parent_id, u16 parent_idx);
    void borrow_from_right_internal(PageId node_id, PageId right_id, PageId parent_id, u16 parent_idx);
    void merge_internal_nodes(PageId left_id, PageId right_id, PageId parent_id, u16 parent_idx);
    PageId alloc_page_id();
    PageId meta_page_id() const;
    void save_meta();
    void init_leaf_page(PageId page_id);
    PageId leftmost_leaf() const;

    static IndexKey read_key_from_slot(const byte* slot);
    static bool write_key_to_slot(byte* slot, const IndexKey& key);

    u16 leaf_num_keys(const Page* page) const;
    IndexKey leaf_key(const Page* page, u16 idx) const;
    RecordId leaf_rid(const Page* page, u16 idx) const;
    PageId leaf_next(const Page* page) const;
    void leaf_set_keys(Page* page, u16 num_keys);
    bool leaf_set_key(Page* page, u16 idx, const IndexKey& key);
    void leaf_set_rid(Page* page, u16 idx, const RecordId& rid);
    void leaf_set_next(Page* page, PageId next);

    u16 internal_num_keys(const Page* page) const;
    IndexKey internal_key(const Page* page, u16 idx) const;
    PageId internal_child(const Page* page, u16 idx) const;
    void internal_set_num_keys(Page* page, u16 n);
    bool internal_set_key(Page* page, u16 idx, const IndexKey& key);
    void internal_set_child(Page* page, u16 idx, PageId child);

    u32 index_id_;
    PageId root_page_id_;
    BufferPool* pool_;
    u32 next_page_num_;
    mutable RwLock tree_latch_;  // W8: Per-tree latch for concurrent safety

    // Monotonic-insert fast path. Bulk loads (especially serial primary keys)
    // hammer the rightmost leaf — every full root-to-leaf descent is wasted.
    // We cache the most recent leaf id we successfully inserted into; if the
    // next key is >= that leaf's first key, we try it before paying the full
    // descent. The cache is invalidated on any split that retargets the
    // rightmost edge.
    PageId hot_leaf_id_ = kNullPageId;
    IndexKey hot_leaf_first_key_;       // smallest key on hot_leaf_id_
    PageId hot_leaf_parent_id_ = kNullPageId;
};

} // namespace minidb
