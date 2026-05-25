/**
 * @file btree.cpp
 * @brief B+ Tree implementation — multi-type key, correct parent tracking, page unpin
 */
#include "index/btree.h"
#include "storage/page.h"
#include "common/config.h"
#include "container/hash_map.h"
#include <cstring>
#include <cstdio>

namespace minidb {

BPlusTree::BPlusTree(u32 index_id, BufferPool* pool)
    : index_id_(index_id), root_page_id_(kNullPageId), pool_(pool), next_page_num_(1) {}

PageId BPlusTree::meta_page_id() const {
    return make_page_id(index_id_, 0);
}

// ============================================================
// alloc_page_id
// ============================================================

PageId BPlusTree::alloc_page_id() {
    PageId pid = make_page_id(index_id_, next_page_num_++);
    save_meta();
    return pid;
}

void BPlusTree::save_meta() {
    PageId meta_id = meta_page_id();
    auto result = pool_->fetch_page(meta_id);
    if (!result.ok()) return;

    Page* page = result.value();
    BTreeMetaPage meta;
    meta.root_page_id = root_page_id_;
    meta.next_page_num = next_page_num_;
    meta.reserved = 0;
    std::memcpy(page->data() + kPageHeaderSize, &meta, sizeof(meta));
    pool_->mark_dirty(meta_id);
    pool_->unpin_page(meta_id);
}

void BPlusTree::init_leaf_page(PageId page_id) {
    auto result = pool_->new_page(page_id, PageType::kIndexData);
    if (!result.ok()) return;

    Page* page = result.value();
    page->init(page_id, PageType::kIndexData);
    byte* data = page->data();
    data[kPageHeaderSize] = 1;
    std::memset(data + kPageHeaderSize + 1, 0, 7);
    std::memset(data + kPageSize - sizeof(u64), 0, sizeof(u64));
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);
}

bool BPlusTree::open() {
    PageId meta_id = meta_page_id();
    auto result = pool_->fetch_page(meta_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    if (page->header()->page_type != static_cast<u16>(PageType::kIndexMeta) ||
        page->header()->page_id != meta_id) {
        pool_->unpin_page(meta_id);
        return false;
    }

    BTreeMetaPage meta;
    std::memcpy(&meta, page->data() + kPageHeaderSize, sizeof(meta));
    pool_->unpin_page(meta_id);

    root_page_id_ = meta.root_page_id;
    next_page_num_ = meta.next_page_num > 1 ? meta.next_page_num : 1;
    return root_page_id_ != kNullPageId;
}

// ============================================================
// create
// ============================================================

void BPlusTree::create() {
    PageId meta_id = meta_page_id();
    auto meta_result = pool_->new_page(meta_id, PageType::kIndexMeta);
    if (!meta_result.ok()) return;
    Page* meta_page = meta_result.value();
    meta_page->init(meta_id, PageType::kIndexMeta);
    pool_->mark_dirty(meta_id);
    pool_->unpin_page(meta_id);

    next_page_num_ = 1;
    PageId root_id = alloc_page_id();
    init_leaf_page(root_id);
    root_page_id_ = root_id;
    save_meta();
}

// ============================================================
// Key slot read/write (type-aware)
// ============================================================

IndexKey BPlusTree::read_key_from_slot(const byte* slot) {
    return IndexKey::decode(slot, kIndexKeyDataSize);
}

bool BPlusTree::write_key_to_slot(byte* slot, const IndexKey& key) {
    return key.encode(slot, kIndexKeyDataSize);
}

// ============================================================
// insert
// ============================================================

bool BPlusTree::insert(const IndexKey& key, const RecordId& rid) {
    if (!key.fits(kIndexKeyDataSize)) return false;
    WriteGuard guard(tree_latch_);
    if (root_page_id_ == kNullPageId) {
        create();
    }

    PageId parent_id = kNullPageId;
    PageId leaf_id = kNullPageId;

    // Fast path for monotonic / append-heavy workloads: if we have a cached
    // "hot leaf" from the previous insert and the new key still belongs
    // beyond that leaf's smallest key, jump straight to it and skip the
    // O(log N) root-to-leaf descent. The leaf itself still binary-searches
    // for the insert slot, so out-of-order keys within the leaf are fine.
    if (hot_leaf_id_ != kNullPageId &&
        !hot_leaf_first_key_.empty() &&
        key >= hot_leaf_first_key_) {
        auto pg_result = pool_->fetch_page(hot_leaf_id_);
        if (pg_result.ok()) {
            Page* page = pg_result.value();
            // Make sure the cached leaf is still a leaf (could have changed
            // through a concurrent rebuild) and that the next leaf either
            // doesn't exist or has a strictly greater first key — otherwise
            // the new key really belongs further right.
            bool usable = page->data()[kPageHeaderSize] == 1;
            if (usable) {
                u16 n = leaf_num_keys(page);
                PageId next_leaf = leaf_next(page);
                if (next_leaf != kNullPageId && n > 0) {
                    auto next_res = pool_->fetch_page(next_leaf);
                    if (next_res.ok()) {
                        IndexKey first_next = leaf_key(next_res.value(), 0);
                        if (key >= first_next) usable = false;
                        pool_->unpin_page(next_leaf);
                    }
                }
            }
            pool_->unpin_page(hot_leaf_id_);
            if (usable) {
                leaf_id = hot_leaf_id_;
                parent_id = hot_leaf_parent_id_;
            }
        }
    }

    // Slow path: full descent from root. find_leaf_with_parent collapses the
    // descent + parent lookup into one pass.
    if (leaf_id == kNullPageId) {
        leaf_id = find_leaf_with_parent(key, &parent_id);
        if (leaf_id == kNullPageId) {
            leaf_id = find_leaf(key);
            parent_id = find_parent(root_page_id_, leaf_id);
        }
    }

    u16 num_keys_after = 0;
    if (!insert_into_leaf(leaf_id, key, rid, &num_keys_after)) return false;

    if (num_keys_after > kIndexMaxKeys) {
        // Splits restructure the rightmost edge; invalidate the hot cache.
        hot_leaf_id_ = kNullPageId;
        hot_leaf_first_key_ = IndexKey();
        hot_leaf_parent_id_ = kNullPageId;
        return split_leaf(leaf_id, parent_id);
    }

    // Successful insert without split — update the hot-leaf cache to this
    // leaf so the next monotonic insert can skip the descent.
    hot_leaf_id_ = leaf_id;
    hot_leaf_parent_id_ = parent_id;
    auto pg_result = pool_->fetch_page(leaf_id);
    if (pg_result.ok()) {
        Page* page = pg_result.value();
        if (leaf_num_keys(page) > 0) {
            hot_leaf_first_key_ = leaf_key(page, 0);
        }
        pool_->unpin_page(leaf_id);
    }
    return true;
}

// ============================================================
// remove
// ============================================================

bool BPlusTree::remove(const IndexKey& key, const RecordId& rid) {
    WriteGuard guard(tree_latch_);
    if (root_page_id_ == kNullPageId) return false;

    // Find leaf and its parent in a single pass
    PageId leaf_id = kNullPageId;
    PageId current = root_page_id_;
    HashMap<PageId, bool> visited;
    u32 hops = 0;

    while (current != kNullPageId && hops < kMaxPageChainHops) {
        if (visited.find(current)) break;
        visited.insert(current, true);
        hops++;
        auto result = pool_->fetch_page(current);
        if (!result.ok()) break;

        Page* page = result.value();
        if (page->data()[kPageHeaderSize] == 1) {
            pool_->unpin_page(current);
            leaf_id = current;
            break;
        }

        u16 n = internal_num_keys(page);
        PageId child = internal_child(page, 0);
        for (u16 i = 0; i < n; i++) {
            IndexKey k = internal_key(page, i);
            if (key <= k) {
                child = internal_child(page, i);
                break;
            }
            child = internal_child(page, i + 1);
        }
        pool_->unpin_page(current);
        current = child;
    }

    while (leaf_id != kNullPageId) {
        auto result = pool_->fetch_page(leaf_id, true);
        if (!result.ok()) return false;
        Page* leaf = result.value();
        u16 n = leaf_num_keys(leaf);
        PageId next = leaf_next(leaf);
        bool found_here = false;
        bool stop_after_this = false;

        for (u16 i = 0; i < n; i++) {
            IndexKey current_key = leaf_key(leaf, i);
            if (current_key > key) {
                stop_after_this = true;
                break;
            }
            if (current_key == key) {
                RecordId current_rid = leaf_rid(leaf, i);
                bool rid_matches = (rid.page_id == kNullPageId) || (current_rid == rid);
                if (rid_matches) {
                    found_here = true;
                    break;
                }
            }
        }

        pool_->unpin_page(leaf_id);
        if (found_here) return remove_from_leaf(leaf_id, key, rid);
        if (stop_after_this) return false;
        leaf_id = next;
    }
    return false;
}

bool BPlusTree::remove_from_leaf(PageId leaf_id, const IndexKey& key, const RecordId& rid) {
    auto result = pool_->fetch_page(leaf_id);
    if (!result.ok()) return false;

    Page* leaf = result.value();
    u16 n = leaf_num_keys(leaf);

    for (u16 i = 0; i < n; i++) {
        RecordId current_rid = leaf_rid(leaf, i);
        bool rid_matches = (rid.page_id == kNullPageId) || (current_rid == rid);
        if (leaf_key(leaf, i) == key && rid_matches) {
            // Shift entries left
            byte* data = leaf->data();
            u32 dst = kIndexKeyStart + i * kIndexLeafSlotSize;
            u32 src = dst + kIndexLeafSlotSize;
            u32 bytes = static_cast<u32>(n - i - 1) * kIndexLeafSlotSize;
            if (bytes > 0) std::memmove(data + dst, data + src, bytes);
            leaf_set_keys(leaf, n - 1);
            pool_->mark_dirty(leaf_id);

            // Update parent separator if we removed the first key
            if (i == 0 && n > 1) {
                PageId pid = find_parent(root_page_id_, leaf_id);
                if (pid != kNullPageId) {
                    auto p_result = pool_->fetch_page(pid);
                    if (p_result.ok()) {
                        Page* parent_page = p_result.value();
                        u16 pn = internal_num_keys(parent_page);
                        for (u16 j = 1; j <= pn; j++) {
                            if (internal_child(parent_page, j) == leaf_id) {
                                internal_set_key(parent_page, j - 1, leaf_key(leaf, 0));
                                pool_->mark_dirty(pid);
                                break;
                            }
                        }
                        pool_->unpin_page(pid);
                    }
                }
            }

            u16 new_n = n - 1;
            pool_->unpin_page(leaf_id);

            if (new_n == 0 && root_page_id_ == leaf_id) {
                root_page_id_ = kNullPageId;
                save_meta();
            } else if (new_n == 0) {
                unlink_empty_leaf(leaf_id);
            }
            if (root_page_id_ != kNullPageId) refresh_internal_separators(root_page_id_);
            return true;
        }
    }
    pool_->unpin_page(leaf_id);
    return false;
}

void BPlusTree::rebalance_after_remove(PageId node_id, PageId parent_id) {
    auto p_result = pool_->fetch_page(parent_id);
    if (!p_result.ok()) return;
    Page* parent = p_result.value();
    u16 pn = internal_num_keys(parent);

    // Find node's index in parent
    u16 node_idx = 0;
    for (u16 i = 0; i <= pn; i++) {
        if (internal_child(parent, i) == node_id) {
            node_idx = i;
            break;
        }
    }
    // NOTE: parent remains pinned — sibling lookups below read from it.

    // Determine if node is leaf or internal
    auto n_result = pool_->fetch_page(node_id);
    if (!n_result.ok()) { pool_->unpin_page(parent_id); return; }
    Page* node = n_result.value();
    bool is_leaf = (node->data()[kPageHeaderSize] == 1);
    u16 n = is_leaf ? leaf_num_keys(node) : internal_num_keys(node);
    pool_->unpin_page(node_id);

    static constexpr u16 kMinKeys = kIndexMaxKeys / 2;
    if (n >= kMinKeys) { pool_->unpin_page(parent_id); return; }

    // Try borrow from left sibling
    if (node_idx > 0) {
        PageId left_id = internal_child(parent, node_idx - 1);
        auto l_result = pool_->fetch_page(left_id);
        if (l_result.ok()) {
            Page* left = l_result.value();
            u16 left_n = is_leaf ? leaf_num_keys(left) : internal_num_keys(left);
            pool_->unpin_page(left_id);
            if (left_n > kMinKeys) {
                pool_->unpin_page(parent_id);
                if (is_leaf)
                    borrow_from_left_leaf(node_id, left_id, parent_id, node_idx - 1);
                else
                    borrow_from_left_internal(node_id, left_id, parent_id, node_idx - 1);
                return;
            }
        }
    }

    // Try borrow from right sibling
    if (node_idx < pn) {
        PageId right_id = internal_child(parent, node_idx + 1);
        auto r_result = pool_->fetch_page(right_id);
        if (r_result.ok()) {
            Page* right = r_result.value();
            u16 right_n = is_leaf ? leaf_num_keys(right) : internal_num_keys(right);
            pool_->unpin_page(right_id);
            if (right_n > kMinKeys) {
                pool_->unpin_page(parent_id);
                if (is_leaf)
                    borrow_from_right_leaf(node_id, right_id, parent_id, node_idx);
                else
                    borrow_from_right_internal(node_id, right_id, parent_id, node_idx);
                return;
            }
        }
    }

    // Merge with a sibling — save sibling IDs before unpinning parent
    PageId merge_left = kNullPageId;
    PageId merge_right = kNullPageId;
    u16 merge_parent_idx = 0;
    if (node_idx > 0) {
        merge_left = internal_child(parent, node_idx - 1);
        merge_right = node_id;
        merge_parent_idx = node_idx - 1;
    } else if (node_idx < pn) {
        merge_left = node_id;
        merge_right = internal_child(parent, node_idx + 1);
        merge_parent_idx = node_idx;
    }
    pool_->unpin_page(parent_id);

    if (merge_left != kNullPageId) {
        if (is_leaf)
            merge_leaves(merge_left, merge_right, parent_id, merge_parent_idx);
        else
            merge_internal_nodes(merge_left, merge_right, parent_id, merge_parent_idx);
    }
}

void BPlusTree::unlink_empty_leaf(PageId leaf_id) {
    PageId next_leaf = kNullPageId;
    {
        auto leaf_result = pool_->fetch_page(leaf_id);
        if (leaf_result.ok()) {
            Page* leaf = leaf_result.value();
            next_leaf = leaf_next(leaf);
            pool_->unpin_page(leaf_id);
        }
    }

    PageId prev_leaf = kNullPageId;
    PageId cur = leftmost_leaf();
    HashMap<PageId, bool> visited;
    u32 hops = 0;
    while (cur != kNullPageId && cur != leaf_id && hops < kMaxPageChainHops) {
        if (visited.find(cur)) break;
        visited.insert(cur, true);
        auto result = pool_->fetch_page(cur);
        if (!result.ok()) break;
        Page* page = result.value();
        PageId next = leaf_next(page);
        pool_->unpin_page(cur);
        prev_leaf = cur;
        cur = next;
        hops++;
    }
    if (cur == leaf_id && prev_leaf != kNullPageId) {
        auto prev_result = pool_->fetch_page(prev_leaf);
        if (prev_result.ok()) {
            Page* prev = prev_result.value();
            leaf_set_next(prev, next_leaf);
            pool_->mark_dirty(prev_leaf);
            pool_->unpin_page(prev_leaf);
        }
    }

    PageId parent_id = find_parent(root_page_id_, leaf_id);
    if (parent_id == kNullPageId) return;
    auto parent_result = pool_->fetch_page(parent_id);
    if (!parent_result.ok()) return;
    Page* parent = parent_result.value();
    u16 pn = internal_num_keys(parent);
    u16 child_idx = 0;
    bool found = false;
    for (u16 i = 0; i <= pn; i++) {
        if (internal_child(parent, i) == leaf_id) {
            child_idx = i;
            found = true;
            break;
        }
    }
    if (!found) {
        pool_->unpin_page(parent_id);
        return;
    }

    if (pn == 0) {
        pool_->unpin_page(parent_id);
        return;
    }

    if (child_idx == 0) {
        for (u16 i = 0; i < pn - 1; i++) {
            internal_set_key(parent, i, internal_key(parent, i + 1));
        }
        for (u16 i = 0; i < pn; i++) {
            internal_set_child(parent, i, internal_child(parent, i + 1));
        }
    } else {
        for (u16 i = child_idx - 1; i < pn - 1; i++) {
            internal_set_key(parent, i, internal_key(parent, i + 1));
        }
        for (u16 i = child_idx; i < pn; i++) {
            internal_set_child(parent, i, internal_child(parent, i + 1));
        }
    }
    internal_set_num_keys(parent, pn - 1);
    PageId only_child = internal_child(parent, 0);
    pool_->mark_dirty(parent_id);
    u16 new_pn = pn - 1;
    pool_->unpin_page(parent_id);

    if (new_pn == 0 && root_page_id_ == parent_id) {
        root_page_id_ = only_child;
        save_meta();
    }
}

bool BPlusTree::first_key_in_subtree(PageId page_id, IndexKey* key) const {
    if (page_id == kNullPageId || !key) return false;
    PageId current = page_id;
    HashMap<PageId, bool> visited;
    u32 hops = 0;
    while (current != kNullPageId && hops < kMaxPageChainHops) {
        if (visited.find(current)) return false;
        visited.insert(current, true);
        auto result = const_cast<BufferPool*>(pool_)->fetch_page(current, true);
        if (!result.ok()) return false;
        Page* page = result.value();
        bool is_leaf = page->data()[kPageHeaderSize] == 1;
        if (is_leaf) {
            u16 n = leaf_num_keys(page);
            if (n == 0) {
                const_cast<BufferPool*>(pool_)->unpin_page(current);
                return false;
            }
            *key = leaf_key(page, 0);
            const_cast<BufferPool*>(pool_)->unpin_page(current);
            return true;
        }
        PageId child = internal_child(page, 0);
        const_cast<BufferPool*>(pool_)->unpin_page(current);
        current = child;
        hops++;
    }
    return false;
}

void BPlusTree::refresh_internal_separators(PageId page_id) {
    if (page_id == kNullPageId) return;
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return;
    Page* page = result.value();
    if (page->data()[kPageHeaderSize] == 1) {
        pool_->unpin_page(page_id);
        return;
    }
    u16 n = internal_num_keys(page);
    Vector<PageId> children;
    children.reserve(static_cast<u32>(n) + 1);
    for (u16 i = 0; i <= n; i++) children.push_back(internal_child(page, i));
    pool_->unpin_page(page_id);

    for (u16 i = 0; i <= n; i++) refresh_internal_separators(children[i]);

    auto second_result = pool_->fetch_page(page_id);
    if (!second_result.ok()) return;
    page = second_result.value();
    for (u16 i = 0; i < n; i++) {
        IndexKey sep;
        if (first_key_in_subtree(children[i + 1], &sep)) {
            internal_set_key(page, i, sep);
        }
    }
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);
}

// ============================================================
// Leaf borrow/merge
// ============================================================

void BPlusTree::borrow_from_left_leaf(PageId leaf_id, PageId left_id,
                                       PageId parent_id, u16 parent_idx) {
    auto l_result = pool_->fetch_page(left_id);
    auto r_result = pool_->fetch_page(leaf_id);
    if (!l_result.ok() || !r_result.ok()) return;

    Page* left = l_result.value();
    Page* leaf = r_result.value();
    u16 ln = leaf_num_keys(left);

    // Move last entry from left to front of leaf
    byte* leaf_data = leaf->data();
    u32 leaf_src = kIndexKeyStart;
    u32 leaf_dst = kIndexKeyStart + kIndexLeafSlotSize;
    u32 leaf_bytes = static_cast<u32>(leaf_num_keys(leaf)) * kIndexLeafSlotSize;
    if (leaf_bytes > 0) std::memmove(leaf_data + leaf_dst, leaf_data + leaf_src, leaf_bytes);

    leaf_set_key(leaf, 0, leaf_key(left, ln - 1));
    leaf_set_rid(leaf, 0, leaf_rid(left, ln - 1));
    leaf_set_keys(leaf, leaf_num_keys(leaf) + 1);
    leaf_set_keys(left, ln - 1);

    // Update parent separator
    auto p_result = pool_->fetch_page(parent_id);
    if (p_result.ok()) {
        Page* parent = p_result.value();
        internal_set_key(parent, parent_idx, leaf_key(leaf, 0));
        pool_->mark_dirty(parent_id);
        pool_->unpin_page(parent_id);
    }

    pool_->mark_dirty(left_id);
    pool_->mark_dirty(leaf_id);
    pool_->unpin_page(left_id);
    pool_->unpin_page(leaf_id);
}

void BPlusTree::borrow_from_right_leaf(PageId leaf_id, PageId right_id,
                                        PageId parent_id, u16 parent_idx) {
    auto l_result = pool_->fetch_page(leaf_id);
    auto r_result = pool_->fetch_page(right_id);
    if (!l_result.ok() || !r_result.ok()) return;

    Page* leaf = l_result.value();
    Page* right = r_result.value();
    u16 ln = leaf_num_keys(leaf);

    // Append first entry from right to leaf
    leaf_set_key(leaf, ln, leaf_key(right, 0));
    leaf_set_rid(leaf, ln, leaf_rid(right, 0));
    leaf_set_keys(leaf, ln + 1);

    // Shift right entries left
    byte* right_data = right->data();
    u32 r_dst = kIndexKeyStart;
    u32 r_src = kIndexKeyStart + kIndexLeafSlotSize;
    u32 r_bytes = static_cast<u32>(leaf_num_keys(right) - 1) * kIndexLeafSlotSize;
    if (r_bytes > 0) std::memmove(right_data + r_dst, right_data + r_src, r_bytes);
    leaf_set_keys(right, leaf_num_keys(right) - 1);

    // Update parent separator
    auto p_result = pool_->fetch_page(parent_id);
    if (p_result.ok()) {
        Page* parent = p_result.value();
        internal_set_key(parent, parent_idx, leaf_key(right, 0));
        pool_->mark_dirty(parent_id);
        pool_->unpin_page(parent_id);
    }

    pool_->mark_dirty(leaf_id);
    pool_->mark_dirty(right_id);
    pool_->unpin_page(leaf_id);
    pool_->unpin_page(right_id);
}

void BPlusTree::merge_leaves(PageId left_id, PageId right_id,
                              PageId parent_id, u16 parent_idx) {
    auto l_result = pool_->fetch_page(left_id);
    auto r_result = pool_->fetch_page(right_id);
    if (!l_result.ok() || !r_result.ok()) return;

    Page* left = l_result.value();
    Page* right = r_result.value();
    u16 ln = leaf_num_keys(left);
    u16 rn = leaf_num_keys(right);

    // Copy all entries from right to end of left
    byte* left_data = left->data();
    byte* right_data = right->data();
    u32 dst = kIndexKeyStart + ln * kIndexLeafSlotSize;
    u32 src = kIndexKeyStart;
    u32 bytes = static_cast<u32>(rn) * kIndexLeafSlotSize;
    std::memcpy(left_data + dst, right_data + src, bytes);
    leaf_set_keys(left, ln + rn);
    leaf_set_next(left, leaf_next(right));

    pool_->mark_dirty(left_id);
    pool_->unpin_page(left_id);
    pool_->unpin_page(right_id);

    // Remove separator from parent
    auto p_result = pool_->fetch_page(parent_id);
    if (!p_result.ok()) return;
    Page* parent = p_result.value();
    u16 pn = internal_num_keys(parent);

    for (u16 i = parent_idx; i < pn - 1; i++) {
        internal_set_key(parent, i, internal_key(parent, i + 1));
        internal_set_child(parent, i + 1, internal_child(parent, i + 2));
    }
    internal_set_num_keys(parent, pn - 1);
    pool_->mark_dirty(parent_id);
    u16 new_pn = pn - 1;
    pool_->unpin_page(parent_id);

    // Propagate up if parent is underfull
    static constexpr u16 kMinInternalKeys = kIndexMaxKeys / 2;
    if (new_pn < kMinInternalKeys && root_page_id_ != parent_id) {
        PageId grandparent = find_parent(root_page_id_, parent_id);
        if (grandparent != kNullPageId) {
            rebalance_after_remove(parent_id, grandparent);
        }
    } else if (new_pn == 0 && root_page_id_ != parent_id) {
        // Parent is empty — re-fetch to read its only child safely
        auto pp_result = pool_->fetch_page(parent_id);
        if (pp_result.ok()) {
            Page* pp = pp_result.value();
            PageId only_child = internal_child(pp, 0);
            pool_->unpin_page(parent_id);
            root_page_id_ = only_child;
            save_meta();
        }
    }
}

// ============================================================
// Internal node borrow/merge
// ============================================================

void BPlusTree::borrow_from_left_internal(PageId node_id, PageId left_id,
                                           PageId parent_id, u16 parent_idx) {
    auto l_result = pool_->fetch_page(left_id);
    auto r_result = pool_->fetch_page(node_id);
    auto p_result = pool_->fetch_page(parent_id);
    if (!l_result.ok() || !r_result.ok() || !p_result.ok()) return;

    Page* left = l_result.value();
    Page* node = r_result.value();
    Page* parent = p_result.value();
    u16 ln = internal_num_keys(left);
    u16 nn = internal_num_keys(node);

    // Move parent separator down to front of node
    // Move left's last key up to parent
    // Shift node entries right
    byte* node_data = node->data();
    u32 n_dst = kIndexKeyStart + kIndexInternalSlotSize;
    u32 n_src = kIndexKeyStart;
    u32 n_bytes = static_cast<u32>(nn) * kIndexInternalSlotSize;
    if (n_bytes > 0) std::memmove(node_data + n_dst, node_data + n_src, n_bytes);

    internal_set_key(node, 0, internal_key(parent, parent_idx));
    internal_set_child(node, 0, internal_child(left, ln));
    internal_set_num_keys(node, nn + 1);

    // Move left's last key to parent
    internal_set_key(parent, parent_idx, internal_key(left, ln - 1));
    internal_set_num_keys(left, ln - 1);

    pool_->mark_dirty(parent_id);
    pool_->mark_dirty(left_id);
    pool_->mark_dirty(node_id);
    pool_->unpin_page(parent_id);
    pool_->unpin_page(left_id);
    pool_->unpin_page(node_id);
}

void BPlusTree::borrow_from_right_internal(PageId node_id, PageId right_id,
                                            PageId parent_id, u16 parent_idx) {
    auto l_result = pool_->fetch_page(node_id);
    auto r_result = pool_->fetch_page(right_id);
    auto p_result = pool_->fetch_page(parent_id);
    if (!l_result.ok() || !r_result.ok() || !p_result.ok()) return;

    Page* node = l_result.value();
    Page* right = r_result.value();
    Page* parent = p_result.value();
    u16 nn = internal_num_keys(node);
    u16 rn = internal_num_keys(right);

    // Append parent separator and right's first child to node
    internal_set_key(node, nn, internal_key(parent, parent_idx));
    internal_set_child(node, nn + 1, internal_child(right, 0));
    internal_set_num_keys(node, nn + 1);

    // Move right's first key to parent, shift right keys/children left.
    // The rightmost child lives just beyond the last key slot, so preserve it
    // before moving slots [1..rn-1] over [0..rn-2].
    internal_set_key(parent, parent_idx, internal_key(right, 0));
    PageId rightmost_child = internal_child(right, rn);
    byte* right_data = right->data();
    u32 r_dst = kIndexKeyStart;
    u32 r_src = kIndexKeyStart + kIndexInternalSlotSize;
    u32 r_bytes = static_cast<u32>(rn - 1) * kIndexInternalSlotSize;
    if (r_bytes > 0) std::memmove(right_data + r_dst, right_data + r_src, r_bytes);
    internal_set_child(right, rn - 1, rightmost_child);
    internal_set_num_keys(right, rn - 1);

    pool_->mark_dirty(parent_id);
    pool_->mark_dirty(node_id);
    pool_->mark_dirty(right_id);
    pool_->unpin_page(parent_id);
    pool_->unpin_page(node_id);
    pool_->unpin_page(right_id);
}

void BPlusTree::merge_internal_nodes(PageId left_id, PageId right_id,
                                      PageId parent_id, u16 parent_idx) {
    auto l_result = pool_->fetch_page(left_id);
    auto r_result = pool_->fetch_page(right_id);
    auto p_result = pool_->fetch_page(parent_id);
    if (!l_result.ok() || !r_result.ok() || !p_result.ok()) return;

    Page* left = l_result.value();
    Page* right = r_result.value();
    Page* parent = p_result.value();
    u16 ln = internal_num_keys(left);
    u16 rn = internal_num_keys(right);

    // Pull down parent separator
    internal_set_key(left, ln, internal_key(parent, parent_idx));
    internal_set_child(left, ln + 1, internal_child(right, 0));

    // Copy right's keys[0..rn-1] and children[1..rn] to left
    for (u16 i = 0; i < rn; i++) {
        internal_set_key(left, ln + 1 + i, internal_key(right, i));
        internal_set_child(left, ln + 2 + i, internal_child(right, i + 1));
    }
    internal_set_num_keys(left, ln + rn + 1);

    pool_->mark_dirty(left_id);
    pool_->unpin_page(left_id);
    pool_->unpin_page(right_id);

    // Remove separator from parent (already fetched as parent)
    u16 pn = internal_num_keys(parent);

    for (u16 i = parent_idx; i < pn - 1; i++) {
        internal_set_key(parent, i, internal_key(parent, i + 1));
        internal_set_child(parent, i + 1, internal_child(parent, i + 2));
    }
    internal_set_num_keys(parent, pn - 1);
    pool_->mark_dirty(parent_id);
    u16 new_pn = pn - 1;
    pool_->unpin_page(parent_id);

    // Propagate up if parent is underfull
    static constexpr u16 kMinInternalKeys = kIndexMaxKeys / 2;
    if (new_pn < kMinInternalKeys && root_page_id_ != parent_id) {
        PageId grandparent = find_parent(root_page_id_, parent_id);
        if (grandparent != kNullPageId) {
            rebalance_after_remove(parent_id, grandparent);
        }
    } else if (new_pn == 0 && root_page_id_ != parent_id) {
        // Parent is empty — re-fetch to read its only child safely
        auto pp_result = pool_->fetch_page(parent_id);
        if (pp_result.ok()) {
            Page* pp = pp_result.value();
            PageId only_child = internal_child(pp, 0);
            pool_->unpin_page(parent_id);
            root_page_id_ = only_child;
            save_meta();
        }
    }
}

// ============================================================
// search / range_search
// ============================================================

Vector<RecordId> BPlusTree::search(const IndexKey& key) {
    ReadGuard guard(tree_latch_);
    Vector<RecordId> results;
    if (root_page_id_ == kNullPageId) return results;

    PageId leaf_id = find_leaf_with_parent(key, nullptr);
    HashMap<PageId, bool> visited;
    u32 hops = 0;
    while (leaf_id != kNullPageId && hops < kMaxPageChainHops) {
        if (visited.find(leaf_id)) break;
        visited.insert(leaf_id, true);
        hops++;
        auto result = pool_->fetch_page(leaf_id, true);
        if (!result.ok()) break;
        Page* page = result.value();
        u16 n = leaf_num_keys(page);
        for (u16 i = 0; i < n; i++) {
            IndexKey k = leaf_key(page, i);
            if (key.column_count() < k.column_count()) {
                if (k.starts_with(key)) results.push_back(leaf_rid(page, i));
                else if (k > key) {
                    pool_->unpin_page(leaf_id);
                    return results;
                }
            } else {
                if (k == key) results.push_back(leaf_rid(page, i));
                else if (k > key) {
                    pool_->unpin_page(leaf_id);
                    return results;
                }
            }
        }
        PageId next = leaf_next(page);
        pool_->unpin_page(leaf_id);
        leaf_id = next;
    }
    return results;
}

Vector<RecordId> BPlusTree::range_search(const IndexKey& low, const IndexKey& high) {
    ReadGuard guard(tree_latch_);
    Vector<RecordId> results;
    if (root_page_id_ == kNullPageId) return results;

    PageId leaf_id = find_leaf_with_parent(low, nullptr);
    HashMap<PageId, bool> visited;
    u32 hops = 0;

    while (leaf_id != kNullPageId && hops < kMaxPageChainHops) {
        if (visited.find(leaf_id)) break;
        visited.insert(leaf_id, true);
        hops++;
        auto result = pool_->fetch_page(leaf_id);
        if (!result.ok()) break;

        Page* page = result.value();
        u16 n = leaf_num_keys(page);
        for (u16 i = 0; i < n; i++) {
            IndexKey k = leaf_key(page, i);
            if (k >= low && k <= high) {
                results.push_back(leaf_rid(page, i));
            }
            if (k > high) {
                pool_->unpin_page(leaf_id);
                return results;
            }
        }

        PageId next = leaf_next(page);
        pool_->unpin_page(leaf_id);
        leaf_id = next;
    }

    return results;
}

u64 BPlusTree::range_count(const IndexKey& low, const IndexKey& high) {
    ReadGuard guard(tree_latch_);
    if (root_page_id_ == kNullPageId) return 0;

    PageId leaf_id = find_leaf_with_parent(low, nullptr);
    HashMap<PageId, bool> visited;
    u32 hops = 0;
    u64 count = 0;
    const bool prefix_mode = (low == high && low.column_count() > 0);

    while (leaf_id != kNullPageId && hops < kMaxPageChainHops) {
        if (visited.find(leaf_id)) break;
        visited.insert(leaf_id, true);
        hops++;
        auto result = pool_->fetch_page(leaf_id, true);
        if (!result.ok()) break;

        Page* page = result.value();
        u16 n = leaf_num_keys(page);
        for (u16 i = 0; i < n; i++) {
            IndexKey k = leaf_key(page, i);
            if (prefix_mode && low.column_count() < k.column_count()) {
                if (k.starts_with(low)) {
                    count++;
                    continue;
                }
                if (k > low) {
                    pool_->unpin_page(leaf_id);
                    return count;
                }
                continue;
            }
            if (k > high) {
                pool_->unpin_page(leaf_id);
                return count;
            }
            if (k >= low) count++;
        }

        PageId next = leaf_next(page);
        pool_->unpin_page(leaf_id);
        leaf_id = next;
    }
    return count;
}

bool BPlusTree::scan_next(const IndexKey& low, const IndexKey& high,
                          PageId* leaf_id, u16* slot_idx,
                          const RecordId* skip_rid, RecordId* rid) {
    IndexKey key;
    return scan_next_entry(low, high, leaf_id, slot_idx, skip_rid, &key, rid);
}

u32 BPlusTree::range_scan_batch(const IndexKey& low, const IndexKey& high,
                                PageId* leaf_id, u16* slot_idx,
                                const RecordId* skip_rid,
                                IndexKey* out_keys, RecordId* out_rids,
                                u32 capacity) {
    if (!leaf_id || !slot_idx || !out_keys || !out_rids || capacity == 0) return 0;
    ReadGuard guard(tree_latch_);
    if (root_page_id_ == kNullPageId) return 0;

    if (*leaf_id == kNullPageId) {
        *leaf_id = find_leaf_with_parent(low, nullptr);
        *slot_idx = 0;
        if (*leaf_id == kNullPageId) return 0;
    }

    auto result = pool_->fetch_page(*leaf_id, true);
    if (!result.ok()) {
        *leaf_id = kNullPageId;
        return 0;
    }
    Page* page = result.value();
    u16 n = leaf_num_keys(page);
    const bool prefix_mode = (low == high && low.column_count() > 0);

    u32 written = 0;
    while (*slot_idx < n && written < capacity) {
        u16 idx = *slot_idx;
        IndexKey k = leaf_key(page, idx);
        if (prefix_mode && low.column_count() < k.column_count()) {
            // Composite-key prefix search: keep walking until either the
            // prefix no longer matches or the key has overshot.
            if (!k.starts_with(low)) {
                if (k > low) {
                    pool_->unpin_page(*leaf_id);
                    *leaf_id = kNullPageId;
                    return written;
                }
                *slot_idx = static_cast<u16>(*slot_idx + 1);
                continue;
            }
        } else {
            if (k > high) {
                pool_->unpin_page(*leaf_id);
                *leaf_id = kNullPageId;
                return written;
            }
            if (k < low) {
                *slot_idx = static_cast<u16>(*slot_idx + 1);
                continue;
            }
        }
        RecordId candidate = leaf_rid(page, idx);
        if (skip_rid && candidate == *skip_rid) {
            *slot_idx = static_cast<u16>(*slot_idx + 1);
            continue;
        }
        out_keys[written] = static_cast<IndexKey&&>(k);
        out_rids[written] = candidate;
        written++;
        *slot_idx = static_cast<u16>(*slot_idx + 1);
    }

    if (*slot_idx >= n) {
        // Exhausted this leaf; reposition cursor at the start of the next.
        PageId next = leaf_next(page);
        pool_->unpin_page(*leaf_id);
        *leaf_id = next;
        *slot_idx = 0;
    } else {
        // Still entries left in the current leaf — drop the pin; caller
        // will resume here on the next batch.
        pool_->unpin_page(*leaf_id);
    }
    return written;
}

bool BPlusTree::scan_next_entry(const IndexKey& low, const IndexKey& high,
                                PageId* leaf_id, u16* slot_idx,
                                const RecordId* skip_rid, IndexKey* key, RecordId* rid) {
    if (!leaf_id || !slot_idx || !rid) return false;
    ReadGuard guard(tree_latch_);
    if (root_page_id_ == kNullPageId) return false;

    if (*leaf_id == kNullPageId) {
        *leaf_id = find_leaf_with_parent(low, nullptr);
        *slot_idx = 0;
    }

    HashMap<PageId, bool> visited;
    u32 hops = 0;
    while (*leaf_id != kNullPageId && hops < kMaxPageChainHops) {
        if (visited.find(*leaf_id)) return false;
        visited.insert(*leaf_id, true);
        hops++;

        auto result = pool_->fetch_page(*leaf_id, true);
        if (!result.ok()) return false;
        Page* page = result.value();
        u16 n = leaf_num_keys(page);

        while (*slot_idx < n) {
            u16 idx = *slot_idx;
            IndexKey k = leaf_key(page, idx);
            bool prefix_mode = (low == high && low.column_count() < k.column_count());
            if (prefix_mode) {
                if (!k.starts_with(low)) {
                    if (k > low) {
                        pool_->unpin_page(*leaf_id);
                        *leaf_id = kNullPageId;
                        return false;
                    }
                    *slot_idx = static_cast<u16>(*slot_idx + 1);
                    continue;
                }
                RecordId candidate = leaf_rid(page, idx);
                if (skip_rid && candidate == *skip_rid) {
                    *slot_idx = static_cast<u16>(*slot_idx + 1);
                    continue;
                }
                if (key) *key = k;
                *rid = candidate;
                pool_->unpin_page(*leaf_id);
                return true;
            }
            if (k > high) {
                pool_->unpin_page(*leaf_id);
                *leaf_id = kNullPageId;
                return false;
            }
            if (k >= low) {
                RecordId candidate = leaf_rid(page, idx);
                if (skip_rid && candidate == *skip_rid) {
                    *slot_idx = static_cast<u16>(*slot_idx + 1);
                    continue;
                }
                if (key) *key = k;
                *rid = candidate;
                pool_->unpin_page(*leaf_id);
                return true;
            }
            *slot_idx = static_cast<u16>(*slot_idx + 1);
        }

        PageId next = leaf_next(page);
        pool_->unpin_page(*leaf_id);
        *leaf_id = next;
        *slot_idx = 0;
    }

    *leaf_id = kNullPageId;
    return false;
}

static bool btree_validation_error(String* error, const char* msg) {
    if (error && error->empty()) *error = String(msg);
    return false;
}

bool BPlusTree::validate_node(PageId page_id, u32 depth, u32* leaf_depth,
                              HashMap<PageId, bool>* visited, String* error) const {
    if (page_id == kNullPageId) return btree_validation_error(error, "null child page");
    if (visited->find(page_id)) return btree_validation_error(error, "cycle in btree");
    visited->insert(page_id, true);

    auto result = const_cast<BufferPool*>(pool_)->fetch_page(page_id);
    if (!result.ok()) return btree_validation_error(error, "failed to fetch btree page");
    Page* page = result.value();
    bool is_leaf = page->data()[kPageHeaderSize] == 1;
    u16 n = is_leaf ? leaf_num_keys(page) : internal_num_keys(page);
    if (n > kIndexMaxKeys) {
        const_cast<BufferPool*>(pool_)->unpin_page(page_id);
        return btree_validation_error(error, "btree node has too many keys");
    }
    if (is_leaf) {
        for (u16 i = 1; i < n; i++) {
            IndexKey prev = leaf_key(page, i - 1);
            IndexKey cur = leaf_key(page, i);
            if (cur < prev) {
                const_cast<BufferPool*>(pool_)->unpin_page(page_id);
                return btree_validation_error(error, "btree keys out of order");
            }
        }
    }

    if (is_leaf) {
        if (*leaf_depth == static_cast<u32>(-1)) {
            *leaf_depth = depth;
        } else if (*leaf_depth != depth) {
            const_cast<BufferPool*>(pool_)->unpin_page(page_id);
            return btree_validation_error(error, "btree leaves at different depths");
        }
        const_cast<BufferPool*>(pool_)->unpin_page(page_id);
        return true;
    }

    Vector<PageId> children;
    children.reserve(static_cast<u32>(n) + 1);
    for (u16 i = 0; i <= n; i++) {
        PageId child = internal_child(page, i);
        if (child == kNullPageId) {
            const_cast<BufferPool*>(pool_)->unpin_page(page_id);
            return btree_validation_error(error, "btree internal node has null child");
        }
        children.push_back(child);
    }
    const_cast<BufferPool*>(pool_)->unpin_page(page_id);

    for (u32 i = 0; i < children.size(); i++) {
        if (!validate_node(children[i], depth + 1, leaf_depth, visited, error)) return false;
    }
    return true;
}

bool BPlusTree::validate_structure(String* error) const {
    ReadGuard guard(tree_latch_);
    if (root_page_id_ == kNullPageId) return true;

    HashMap<PageId, bool> visited;
    u32 leaf_depth = static_cast<u32>(-1);
    if (!validate_node(root_page_id_, 0, &leaf_depth, &visited, error)) return false;

    PageId leaf_id = leftmost_leaf();
    HashMap<PageId, bool> leaf_seen;
    bool have_last = false;
    IndexKey last_key;
    while (leaf_id != kNullPageId) {
        if (leaf_seen.find(leaf_id)) return btree_validation_error(error, "cycle in leaf chain");
        leaf_seen.insert(leaf_id, true);
        auto result = const_cast<BufferPool*>(pool_)->fetch_page(leaf_id, true);
        if (!result.ok()) return btree_validation_error(error, "failed to fetch leaf chain page");
        Page* page = result.value();
        if (page->data()[kPageHeaderSize] != 1) {
            const_cast<BufferPool*>(pool_)->unpin_page(leaf_id);
            return btree_validation_error(error, "leaf chain points to internal page");
        }
        u16 n = leaf_num_keys(page);
        for (u16 i = 0; i < n; i++) {
            IndexKey cur = leaf_key(page, i);
            if (have_last && cur < last_key) {
                const_cast<BufferPool*>(pool_)->unpin_page(leaf_id);
                return btree_validation_error(error, "leaf chain keys out of order");
            }
            last_key = cur;
            have_last = true;
        }
        PageId next = leaf_next(page);
        const_cast<BufferPool*>(pool_)->unpin_page(leaf_id);
        leaf_id = next;
    }
    return true;
}

// ============================================================
// find_leaf — top-down traversal, unpin all internal nodes.
// ============================================================

PageId BPlusTree::find_leaf(const IndexKey& key) const {
    return find_leaf_with_parent(key, nullptr);
}

PageId BPlusTree::leftmost_leaf() const {
    if (root_page_id_ == kNullPageId) return kNullPageId;

    PageId current = root_page_id_;
    HashMap<PageId, bool> visited;
    u32 hops = 0;
    while (current != kNullPageId && hops < kMaxPageChainHops) {
        if (visited.find(current)) break;
        visited.insert(current, true);
        hops++;

        auto result = const_cast<BufferPool*>(pool_)->fetch_page(current);
        if (!result.ok()) return kNullPageId;
        Page* page = result.value();
        if (page->data()[kPageHeaderSize] == 1) {
            const_cast<BufferPool*>(pool_)->unpin_page(current);
            return current;
        }
        PageId next = internal_child(page, 0);
        const_cast<BufferPool*>(pool_)->unpin_page(current);
        current = next;
    }
    return kNullPageId;
}

PageId BPlusTree::find_leaf_with_parent(const IndexKey& key, PageId* parent_id) const {
    PageId current = root_page_id_;
    PageId parent = kNullPageId;
    HashMap<PageId, bool> visited;
    u32 hops = 0;

    while (current != kNullPageId && hops < kMaxPageChainHops) {
        if (visited.find(current)) break;
        visited.insert(current, true);
        hops++;
        auto result = const_cast<BufferPool*>(pool_)->fetch_page(current);
        if (!result.ok()) break;

        Page* page = result.value();

        if (page->data()[kPageHeaderSize] == 1) {
            const_cast<BufferPool*>(pool_)->unpin_page(current);
            if (parent_id) *parent_id = parent;
            return current;
        }

        u16 n = internal_num_keys(page);
        // Binary search for the first separator key K[i] >= search key.
        // We descend into child[i] in that case (left side of equal keys
        // so leaf-chain scans see duplicates split across adjacent leaves).
        // If no such i exists, descend the rightmost child[n].
        // Each comparison still allocates an IndexKey, but log2(n) is far
        // better than n for the fan-out we now achieve after the 512→128
        // key-size reduction (≈57 keys per internal node).
        u16 lo = 0, hi = n;
        while (lo < hi) {
            u16 mid = static_cast<u16>(lo + (hi - lo) / 2);
            IndexKey mid_key = internal_key(page, mid);
            if (key <= mid_key) {
                hi = mid;
            } else {
                lo = static_cast<u16>(mid + 1);
            }
        }
        PageId child = internal_child(page, lo);

        const_cast<BufferPool*>(pool_)->unpin_page(current);
        parent = current;
        current = child;
    }

    if (parent_id) *parent_id = kNullPageId;
    return current;
}

// ============================================================
// insert_into_leaf
// ============================================================

bool BPlusTree::insert_into_leaf(PageId leaf_id, const IndexKey& key, const RecordId& rid,
                                 u16* num_keys_after) {
    auto result = pool_->fetch_page(leaf_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    u16 n = leaf_num_keys(page);

    // Fast path for the bulk-load pattern: if the new key is >= the current
    // largest key, append at the end without scanning. Each leaf_key() call
    // performs a full IndexKey::decode (heap allocations for Vector<Value>
    // and any VARCHAR strings) — skipping the linear scan saves O(n) decodes
    // per insert when the workload is sequential (common for serial PKs).
    u16 pos = n;
    if (n > 0) {
        IndexKey last_key = leaf_key(page, n - 1);
        if (key >= last_key) {
            pos = n;  // append; no scan
        } else {
            // Binary search the leaf for the correct insertion position.
            // n is small (≤57) but at one heap allocation per decode this is
            // still O(log n) decodes instead of O(n).
            u16 lo = 0, hi = n;
            while (lo < hi) {
                u16 mid = static_cast<u16>(lo + (hi - lo) / 2);
                IndexKey mid_key = leaf_key(page, mid);
                if (key < mid_key) {
                    hi = mid;
                } else {
                    lo = static_cast<u16>(mid + 1);
                }
            }
            pos = lo;
        }
    }

    if (pos < n) {
        byte* data = page->data();
        u32 src = kIndexKeyStart + pos * kIndexLeafSlotSize;
        u32 dst = src + kIndexLeafSlotSize;
        u32 bytes = static_cast<u32>(n - pos) * kIndexLeafSlotSize;
        std::memmove(data + dst, data + src, bytes);
    }

    leaf_set_key(page, pos, key);
    leaf_set_rid(page, pos, rid);
    leaf_set_keys(page, n + 1);
    if (num_keys_after) *num_keys_after = static_cast<u16>(n + 1);

    pool_->mark_dirty(leaf_id);
    pool_->unpin_page(leaf_id);
    return true;
}

// ============================================================
// find_parent — find the internal node holding child pointer (child is a page ID of one of its children)
//
// Using iterative DFS:
// 1) Copy child pointer before unpin, avoid use-after-unpin
// 2) Avoid deep stack from recursion
// ============================================================

PageId BPlusTree::find_parent(PageId root, PageId child) const {
    if (root == child) return kNullPageId;
    BufferPool* pool = const_cast<BufferPool*>(pool_);
    Vector<PageId> stack;
    HashMap<PageId, bool> visited;
    u32 hops = 0;
    stack.push_back(root);

    while (!stack.empty() && hops < kMaxPageChainHops) {
        PageId current = stack.back();
        stack.pop_back();
        if (current == kNullPageId) continue;
        if (visited.find(current)) continue;
        visited.insert(current, true);
        hops++;

        auto result = pool->fetch_page(current);
        if (!result.ok()) continue;

        Page* page = result.value();
        if (page->data()[kPageHeaderSize] == 1) {
            pool->unpin_page(current);
            continue;
        }

        u16 n = internal_num_keys(page);
        Vector<PageId> subs;
        subs.reserve(static_cast<Vector<PageId>::size_type>(n + 1));
        for (u16 i = 0; i <= n; i++) {
            subs.push_back(internal_child(page, i));
        }
        pool->unpin_page(current);

        for (u16 i = 0; i <= n; i++) {
            if (subs[i] == child) return current;
        }

        for (i32 i = static_cast<i32>(n); i >= 0; i--) {
            if (subs[static_cast<u16>(i)] != kNullPageId) {
                stack.push_back(subs[static_cast<u16>(i)]);
            }
        }
    }
    return kNullPageId;
}

// ============================================================
// split_leaf
// ============================================================

bool BPlusTree::split_leaf(PageId leaf_id, PageId parent_id) {
    auto fetch_result = pool_->fetch_page(leaf_id);
    if (!fetch_result.ok()) return false;
    Page* old_page = fetch_result.value();
    u16 n = leaf_num_keys(old_page);
    u16 mid = n / 2;

    PageId new_id = alloc_page_id();
    auto new_result = pool_->new_page(new_id, PageType::kIndexData);
    if (!new_result.ok()) {
        pool_->unpin_page(leaf_id);
        return false;
    }

    Page* new_page = new_result.value();
    new_page->init(new_id, PageType::kIndexData);
    new_page->data()[kPageHeaderSize] = 1;

    u16 new_count = n - mid;
    byte* old_data = old_page->data();
    byte* new_data = new_page->data();
    u32 src = kIndexKeyStart + mid * kIndexLeafSlotSize;
    u32 dst = kIndexKeyStart;
    u32 bytes = static_cast<u32>(new_count) * kIndexLeafSlotSize;
    std::memcpy(new_data + dst, old_data + src, bytes);
    leaf_set_keys(new_page, new_count);
    leaf_set_next(new_page, leaf_next(old_page));
    leaf_set_keys(old_page, mid);
    leaf_set_next(old_page, new_id);

    pool_->mark_dirty(leaf_id);
    pool_->mark_dirty(new_id);

    IndexKey push_key = leaf_key(new_page, 0);
    bool linked = insert_into_parent(parent_id, push_key, leaf_id, new_id);
    if (!linked) {
        std::memcpy(old_data + src, new_data + dst, bytes);
        leaf_set_keys(old_page, n);
        leaf_set_next(old_page, leaf_next(new_page));
        leaf_set_keys(new_page, 0);
        pool_->mark_dirty(leaf_id);
        pool_->mark_dirty(new_id);
    }

    pool_->unpin_page(leaf_id);
    pool_->unpin_page(new_id);
    return linked;
}

// ============================================================
// insert_into_parent
// ============================================================

bool BPlusTree::insert_into_parent(PageId parent_id, const IndexKey& key,
                                    PageId left, PageId right) {
    if (parent_id == kNullPageId) {
        PageId new_root_id = alloc_page_id();
        auto result = pool_->new_page(new_root_id, PageType::kIndexData);
        if (!result.ok()) return false;

        Page* page = result.value();
        page->init(new_root_id, PageType::kIndexData);

        byte* data = page->data();
        data[kPageHeaderSize] = 0;
        internal_set_num_keys(page, 1);
        internal_set_key(page, 0, key);
        internal_set_child(page, 0, left);
        internal_set_child(page, 1, right);

        root_page_id_ = new_root_id;
        save_meta();
        pool_->mark_dirty(new_root_id);
        pool_->unpin_page(new_root_id);
        return true;
    }

    auto fetch_result = pool_->fetch_page(parent_id);
    if (!fetch_result.ok()) return false;
    Page* parent = fetch_result.value();

    u16 n = internal_num_keys(parent);

    u16 pos = n;
    for (u16 i = 0; i < n; i++) {
        if (key < internal_key(parent, i)) {
            pos = i;
            break;
        }
    }

    for (u16 i = n; i > pos; i--) {
        internal_set_key(parent, i, internal_key(parent, i - 1));
        internal_set_child(parent, i + 1, internal_child(parent, i));
    }

    internal_set_key(parent, pos, key);
    internal_set_child(parent, pos + 1, right);
    internal_set_num_keys(parent, n + 1);

    pool_->mark_dirty(parent_id);
    bool need_split = (static_cast<u32>(n) + 1 > kIndexMaxKeys);
    pool_->unpin_page(parent_id);

    if (need_split) {
        split_internal(parent_id);
    }
    return true;
}

// ============================================================
// split_internal
// ============================================================

bool BPlusTree::split_internal(PageId node_id) {
    auto fetch_result = pool_->fetch_page(node_id);
    if (!fetch_result.ok()) return false;
    Page* old_page = fetch_result.value();
    u16 n = internal_num_keys(old_page);
    u16 mid = n / 2;

    IndexKey push_key = internal_key(old_page, mid);
    PageId parent_id = find_parent(root_page_id_, node_id);

    PageId new_id = alloc_page_id();
    auto new_result = pool_->new_page(new_id, PageType::kIndexData);
    if (!new_result.ok()) {
        pool_->unpin_page(node_id);
        return false;
    }

    Page* new_page = new_result.value();
    new_page->init(new_id, PageType::kIndexData);
    new_page->data()[kPageHeaderSize] = 0;

    u16 new_count = n - mid - 1;
    for (u16 i = 0; i < new_count; i++) {
        internal_set_key(new_page, i, internal_key(old_page, mid + 1 + i));
        internal_set_child(new_page, i, internal_child(old_page, mid + 1 + i));
    }
    internal_set_child(new_page, new_count, internal_child(old_page, n));
    internal_set_num_keys(new_page, new_count);

    internal_set_num_keys(old_page, mid);

    pool_->mark_dirty(node_id);
    pool_->mark_dirty(new_id);

    bool linked = insert_into_parent(parent_id, push_key, node_id, new_id);
    if (!linked) {
        for (u16 i = 0; i < new_count; i++) {
            internal_set_key(old_page, mid + 1 + i, internal_key(new_page, i));
            internal_set_child(old_page, mid + 1 + i, internal_child(new_page, i));
        }
        internal_set_child(old_page, n, internal_child(new_page, new_count));
        internal_set_num_keys(old_page, n);
        internal_set_num_keys(new_page, 0);
        pool_->mark_dirty(node_id);
        pool_->mark_dirty(new_id);
    }

    pool_->unpin_page(node_id);
    pool_->unpin_page(new_id);
    return linked;
}

// ============================================================
// Leaf node read/write
// ============================================================

u16 BPlusTree::leaf_num_keys(const Page* page) const {
    u16 n;
    std::memcpy(&n, page->data() + kPageHeaderSize + 1, 2);
    return n;
}

IndexKey BPlusTree::leaf_key(const Page* page, u16 idx) const {
    u32 offset = kIndexKeyStart + idx * kIndexLeafSlotSize;
    return read_key_from_slot(page->data() + offset);
}

RecordId BPlusTree::leaf_rid(const Page* page, u16 idx) const {
    u32 offset = kIndexKeyStart + idx * kIndexLeafSlotSize + kIndexKeyDataSize;
    RecordId rid;
    std::memcpy(&rid.page_id, page->data() + offset, 8);
    std::memcpy(&rid.slot_idx, page->data() + offset + 8, 2);
    return rid;
}

PageId BPlusTree::leaf_next(const Page* page) const {
    PageId next;
    std::memcpy(&next, page->data() + kPageSize - sizeof(u64), sizeof(u64));
    return next;
}

void BPlusTree::leaf_set_keys(Page* page, u16 num_keys) {
    std::memcpy(page->data() + kPageHeaderSize + 1, &num_keys, 2);
}

bool BPlusTree::leaf_set_key(Page* page, u16 idx, const IndexKey& key) {
    u32 offset = kIndexKeyStart + idx * kIndexLeafSlotSize;
    return write_key_to_slot(page->data() + offset, key);
}

void BPlusTree::leaf_set_rid(Page* page, u16 idx, const RecordId& rid) {
    u32 offset = kIndexKeyStart + idx * kIndexLeafSlotSize + kIndexKeyDataSize;
    std::memcpy(page->data() + offset, &rid.page_id, 8);
    std::memcpy(page->data() + offset + 8, &rid.slot_idx, 2);
}

void BPlusTree::leaf_set_next(Page* page, PageId next) {
    std::memcpy(page->data() + kPageSize - sizeof(u64), &next, sizeof(u64));
}

// ============================================================
// Internal-node read/write helpers.
// ============================================================

u16 BPlusTree::internal_num_keys(const Page* page) const {
    u16 n;
    std::memcpy(&n, page->data() + kPageHeaderSize + 1, 2);
    return n;
}

IndexKey BPlusTree::internal_key(const Page* page, u16 idx) const {
    u32 offset = kIndexKeyStart + idx * kIndexInternalSlotSize;
    return read_key_from_slot(page->data() + offset);
}

PageId BPlusTree::internal_child(const Page* page, u16 idx) const {
    u32 offset = kIndexKeyStart + idx * kIndexInternalSlotSize + kIndexKeyDataSize;
    PageId pid;
    std::memcpy(&pid, page->data() + offset, 8);
    return pid;
}

void BPlusTree::internal_set_num_keys(Page* page, u16 n) {
    std::memcpy(page->data() + kPageHeaderSize + 1, &n, 2);
}

bool BPlusTree::internal_set_key(Page* page, u16 idx, const IndexKey& key) {
    u32 offset = kIndexKeyStart + idx * kIndexInternalSlotSize;
    return write_key_to_slot(page->data() + offset, key);
}

void BPlusTree::internal_set_child(Page* page, u16 idx, PageId child) {
    u32 offset = kIndexKeyStart + idx * kIndexInternalSlotSize + kIndexKeyDataSize;
    std::memcpy(page->data() + offset, &child, 8);
}

} // namespace minidb
