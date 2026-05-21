/**
 * @file heap_file.cpp
 * @brief HeapFile implementation — PostgreSQL style: MVCC mark-deleted + page-level pruning
 */
#include "storage/heap_file.h"
#include <cstring>

namespace minidb {

static inline u16 max_align(u16 size) {
    return (size + 7) & ~7;
}

HeapFile::HeapFile(BufferPool* pool, u32 table_id)
    : pool_(pool), table_id_(table_id), meta_loaded_(false),
      meta_dirty_(false), meta_mutations_since_save_(0) {
    std::memset(&meta_, 0, sizeof(HeapMeta));
    meta_.table_id = table_id;
}

// ============================================================
// create
// ============================================================

void HeapFile::create() {
    PageId pid = meta_page_id();
    auto result = pool_->new_page(pid, PageType::kHeapMeta);
    if (!result.ok()) return;

    Page* page = result.value();
    page->init(pid, PageType::kHeapMeta);

    meta_.table_id = table_id_;
    meta_.first_data_page_id = kNullPageId;
    meta_.last_data_page_id = kNullPageId;
    meta_.num_data_pages = 0;
    meta_.num_tuples = 0;
    meta_.num_deleted_tuples = 0;
    meta_loaded_ = true;
    meta_dirty_ = false;
    meta_mutations_since_save_ = 0;

    std::memcpy(page->data() + kPageHeaderSize, &meta_, sizeof(HeapMeta));
    pool_->mark_dirty(pid);
    pool_->unpin_page(pid);
}

// ============================================================
// insert_tuple — prune before insert
// ============================================================

Result<Pair<PageId, SlotIdx>> HeapFile::insert_tuple(const byte* data, u16 length, u64 lsn) {
    LockGuard guard(latch_);
    ensure_meta_loaded();

    // No data pages, create first one
    if (meta_.last_data_page_id == kNullPageId) {
        PageId new_pid = allocate_new_page_id();
        auto result = pool_->new_page(new_pid, PageType::kHeapData);
        if (!result.ok()) return result.error();

        Page* page = result.value();
        page->init(new_pid, PageType::kHeapData);
        if (lsn != 0) pool_->set_page_lsn(new_pid, lsn);
        pool_->mark_dirty(new_pid);
        pool_->unpin_page(new_pid);

        meta_.first_data_page_id = new_pid;
        meta_.last_data_page_id = new_pid;
        meta_.num_data_pages = 1;
    }

    // Attempt insert on last page
    PageId last_pid = meta_.last_data_page_id;
    auto last_result = pool_->fetch_page(last_pid);
    if (!last_result.ok()) return last_result.error();
    Page* last_page = last_result.value();

    // Prune DEAD tuples first
    last_page->prune();
    pool_->mark_dirty(last_pid);

    if (last_page->has_enough_space(length)) {
        SlotIdx slot = last_page->insert_tuple(data, length);
        if (slot != kNullSlot) {
            if (lsn != 0) pool_->set_page_lsn(last_pid, lsn);
            pool_->mark_dirty(last_pid);
            pool_->unpin_page(last_pid);
            meta_.num_tuples++;
            note_meta_changed();
            return Pair<PageId, SlotIdx>(last_pid, slot);
        }
    }

    // Last page full, allocate new page
    pool_->unpin_page(last_pid);
    PageId new_pid = allocate_new_page_id();

    // Update old last page's next_page_id (also set LSN to ensure linked list consistency)
    auto old_result = pool_->fetch_page(last_pid);
    if (old_result.ok()) {
        Page* old_page = old_result.value();
        u64 next_ptr = new_pid;
        std::memcpy(old_page->data() + kPageSize - sizeof(u64),
                    &next_ptr, sizeof(u64));
        if (lsn != 0) pool_->set_page_lsn(last_pid, lsn);
        pool_->mark_dirty(last_pid);
        pool_->unpin_page(last_pid);
    }

    // Initialize new page
    auto new_result = pool_->new_page(new_pid, PageType::kHeapData);
    if (!new_result.ok()) return new_result.error();

    Page* new_page = new_result.value();
    new_page->init(new_pid, PageType::kHeapData);
    if (lsn != 0) pool_->set_page_lsn(new_pid, lsn);

    u64 null_next = kNullPageId;
    std::memcpy(new_page->data() + kPageSize - sizeof(u64),
                &null_next, sizeof(u64));

    SlotIdx slot = new_page->insert_tuple(data, length);
    if (slot == kNullSlot) {
        pool_->unpin_page(new_pid);
        return Status(ErrorCode::kPageFull, "failed to insert tuple into new page");
    }
    pool_->mark_dirty(new_pid);
    pool_->unpin_page(new_pid);

    meta_.last_data_page_id = new_pid;
    meta_.num_data_pages++;
    meta_.num_tuples++;
    note_meta_changed();

    return Pair<PageId, SlotIdx>(new_pid, slot);
}

// ============================================================
// predict_slot — predict the next free slot on a page WITHOUT mutating it.
// ============================================================

SlotIdx HeapFile::predict_slot(Page* page, u16 length) const {
    PageHeader* hdr = page->header();
    u16 num = hdr->num_tuples;
    u16 aligned_len = max_align(length);
    SlotIdx reusable_slot = kNullSlot;

    // Mirrors Page::insert_tuple's slot-selection logic exactly.
    // Strategy 1: reuse reclaimable slot
    for (u16 i = 0; i < num; i++) {
        const LinePointer* lp = page->line_pointer(i);
        if (lp && lp->is_usable()) {
            if (lp->flags == LP_UNUSED && reusable_slot == kNullSlot) {
                reusable_slot = i;
            }
            if (lp->length > 0 && aligned_len <= max_align(lp->length)) {
                return i;
            }
        }
    }

    // Strategy 2: append (reuse a free slot or grow into a new one).
    return (reusable_slot == kNullSlot) ? num : reusable_slot;
}

// ============================================================
// prepare_insert — WAL-first: reserve a slot without modifying the page.
// ============================================================

Result<HeapFile::InsertPlan> HeapFile::prepare_insert(u16 length) {
    latch_.lock();
    ensure_meta_loaded();

    // No data page yet -> a new page will be allocated.
    if (meta_.last_data_page_id == kNullPageId) {
        PageId new_pid = allocate_new_page_id();
        return InsertPlan{new_pid, true, 0};
    }

    // Attempt insert on last page
    PageId last_pid = meta_.last_data_page_id;
    auto last_result = pool_->fetch_page(last_pid);
    if (!last_result.ok()) {
        latch_.unlock();
        return last_result.error();
    }

    Page* last_page = last_result.value();

    // Prune DEAD tuples first to reclaim recyclable space.
    last_page->prune();
    pool_->mark_dirty(last_pid);

    if (last_page->has_enough_space(length)) {
        SlotIdx slot = predict_slot(last_page, length);
        pool_->unpin_page(last_pid);
        return InsertPlan{last_pid, false, slot};
    }

    // Last page is full -> a new page will be allocated.
    pool_->unpin_page(last_pid);
    PageId new_pid = allocate_new_page_id();
    return InsertPlan{new_pid, true, 0};
}

// ============================================================
// commit_insert — WAL-first: commit the insert (set LSN, write data).
// ============================================================

Result<Pair<PageId, SlotIdx>> HeapFile::commit_insert(PageId page_id, bool is_new_page,
                                                       SlotIdx predicted_slot,
                                                       const byte* data, u16 length, u64 lsn) {
    struct UnlockOnExit {
        Mutex& latch;
        ~UnlockOnExit() { latch.unlock(); }
    } unlock{latch_};
    ensure_meta_loaded();

    if (is_new_page) {
        auto new_result = pool_->new_page(page_id, PageType::kHeapData);
        if (!new_result.ok()) return new_result.error();

        Page* new_page = new_result.value();
        new_page->init(page_id, PageType::kHeapData);
        if (lsn != 0) pool_->set_page_lsn(page_id, lsn);

        u64 null_next = kNullPageId;
        std::memcpy(new_page->data() + kPageSize - sizeof(u64), &null_next, sizeof(u64));

        SlotIdx slot = new_page->insert_tuple(data, length);
        if (slot == kNullSlot) {
            pool_->unpin_page(page_id);
            return Status(ErrorCode::kPageFull, "failed to insert tuple into new page");
        }
        pool_->mark_dirty(page_id);
        pool_->unpin_page(page_id);

        // Update old last page's next_page_id (also set LSN to ensure linked list consistency)
        if (meta_.last_data_page_id != kNullPageId) {
            auto old_result = pool_->fetch_page(meta_.last_data_page_id);
            if (old_result.ok()) {
                Page* old_page = old_result.value();
                u64 next_ptr = page_id;
                std::memcpy(old_page->data() + kPageSize - sizeof(u64), &next_ptr, sizeof(u64));
                if (lsn != 0) pool_->set_page_lsn(meta_.last_data_page_id, lsn);
                pool_->mark_dirty(meta_.last_data_page_id);
                pool_->unpin_page(meta_.last_data_page_id);
            }
        }

        meta_.last_data_page_id = page_id;
        if (meta_.first_data_page_id == kNullPageId) meta_.first_data_page_id = page_id;
        meta_.num_data_pages++;
        meta_.num_tuples++;
        note_meta_changed();
        return Pair<PageId, SlotIdx>(page_id, slot);
    }

    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return result.error();

    Page* page = result.value();
    SlotIdx slot = page->insert_tuple_at(data, length, predicted_slot);
    if (slot == kNullSlot) {
        pool_->unpin_page(page_id);
        return Status(ErrorCode::kPageFull, "commit_insert: predicted slot unavailable");
    }
    if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);

    meta_.num_tuples++;
    note_meta_changed();
    return Pair<PageId, SlotIdx>(page_id, slot);
}

// ============================================================
// commit_old_tuple — atomic: set_next_version + mark_deleted + set_lsn,
// done in a single pinned-page operation so the LSN is stamped before unpin.
// ============================================================

bool HeapFile::commit_old_tuple(PageId page_id, SlotIdx slot_idx,
                                PageId next_page, SlotIdx next_slot,
                                u64 xmax, u64 lsn) {
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(slot_idx);
    if (!lp || !lp->is_valid()) {
        pool_->unpin_page(page_id);
        return false;
    }

    if (static_cast<u32>(lp->offset) + 26 > kPageSize || lp->length < 26) {
        pool_->unpin_page(page_id);
        return false;
    }

    // SettingsVersion chain: [next_page 8B][next_slot 2B] at offset +16/+24
    byte* base = page->data() + lp->offset;
    std::memcpy(base + 16, &next_page, 8);
    std::memcpy(base + 24, &next_slot, 2);

    // Settings xmax at offset +8
    std::memcpy(base + 8, &xmax, 8);

    // Stamp the page LSN before unpinning.
    if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);
    return true;
}

// ============================================================
// set_page_lsn — fallback for callers that cannot stamp atomically.
// ============================================================

void HeapFile::set_page_lsn(PageId page_id, u64 lsn) {
    if (lsn == 0) return;
    pool_->set_page_lsn(page_id, lsn);
}

// ============================================================
// prepare_insert_in_page — WAL-first HOT: reserve a same-page insert slot.
// ============================================================

Result<SlotIdx> HeapFile::prepare_insert_in_page(PageId page_id, u16 length) {
    latch_.lock();
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) {
        latch_.unlock();
        return Status(ErrorCode::kIOError);
    }

    Page* page = result.value();

    // Prune DEAD tuples first to free up reclaimable space.
    page->prune();
    pool_->mark_dirty(page_id);

    if (!page->has_enough_space(length)) {
        pool_->unpin_page(page_id);
        latch_.unlock();
        return Status(ErrorCode::kPageFull);
    }

    SlotIdx slot = predict_slot(page, length);
    pool_->unpin_page(page_id);
    return slot;
}

// ============================================================
// commit_insert_in_page — WAL-first HOT: commit a same-page insert.
// ============================================================

Result<Pair<PageId, SlotIdx>> HeapFile::commit_insert_in_page(PageId page_id, SlotIdx slot_idx,
                                                              const byte* data, u16 length, u64 lsn) {
    struct UnlockOnExit {
        Mutex& latch;
        ~UnlockOnExit() { latch.unlock(); }
    } unlock{latch_};
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return Status(ErrorCode::kIOError);

    Page* page = result.value();
    SlotIdx actual_slot = page->insert_tuple_at(data, length, slot_idx);
    if (actual_slot == kNullSlot) {
        pool_->unpin_page(page_id);
        return Status(ErrorCode::kPageFull, "commit_insert_in_page failed");
    }
    if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);

    meta_.num_tuples++;
    note_meta_changed();
    return Pair<PageId, SlotIdx>(page_id, actual_slot);
}

// ============================================================
// insert_tuple_in_page — HOT: insert into a specific page; returns kNullSlot on failure.
// ============================================================

Result<Pair<PageId, SlotIdx>> HeapFile::insert_tuple_in_page(PageId page_id,
                                                             const byte* data, u16 length,
                                                             u64 lsn) {
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return Status(ErrorCode::kIOError);

    Page* page = result.value();

    // Prune DEAD tuples first
    page->prune();
    pool_->mark_dirty(page_id);

    if (!page->has_enough_space(length)) {
        pool_->unpin_page(page_id);
        return Status(ErrorCode::kPageFull);
    }

    SlotIdx slot = page->insert_tuple(data, length);
    if (slot == kNullSlot) {
        pool_->unpin_page(page_id);
        return Status(ErrorCode::kPageFull);
    }
    if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);

    meta_.num_tuples++;
    note_meta_changed();
    return Pair<PageId, SlotIdx>(page_id, slot);
}

// ============================================================
// mark_deleted — MVCC soft-delete: only stamp xmax; line pointer is preserved.
// ============================================================

bool HeapFile::mark_deleted(PageId page_id, SlotIdx slot_idx, u64 xmax, u64 lsn) {
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(slot_idx);
    if (!lp || !lp->is_valid()) {
        pool_->unpin_page(page_id);
        return false;
    }

    if (static_cast<u32>(lp->offset) + 16 > kPageSize || lp->length < 16) {
        pool_->unpin_page(page_id);
        return false;
    }

    // xmax sits at tuple-header offset 8 (after xmin).
    byte* xmax_ptr = page->data() + lp->offset + 8;
    std::memcpy(xmax_ptr, &xmax, 8);

    if (lsn != 0) {
        // page_lsn must only move forward: never overwrite a committed higher LSN.
        u64 current_lsn = page->header()->lsn;
        if (lsn > current_lsn) pool_->set_page_lsn(page_id, lsn);
    }
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);
    return true;
}

bool HeapFile::set_xmin(PageId page_id, SlotIdx slot_idx, u64 xmin, u64 lsn) {
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(slot_idx);
    if (!lp || !lp->is_valid()) {
        pool_->unpin_page(page_id);
        return false;
    }

    if (static_cast<u32>(lp->offset) + 8 > kPageSize || lp->length < 8) {
        pool_->unpin_page(page_id);
        return false;
    }

    byte* xmin_ptr = page->data() + lp->offset;
    std::memcpy(xmin_ptr, &xmin, 8);

    if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);
    return true;
}

bool HeapFile::set_next_version(PageId page_id, SlotIdx slot_idx,
                                PageId next_page, SlotIdx next_slot, u64 lsn) {
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(slot_idx);
    if (!lp || !lp->is_valid()) {
        pool_->unpin_page(page_id);
        return false;
    }

    if (static_cast<u32>(lp->offset) + 26 > kPageSize || lp->length < 26) {
        pool_->unpin_page(page_id);
        return false;
    }

    byte* nv_ptr = page->data() + lp->offset + 16;
    std::memcpy(nv_ptr, &next_page, 8);
    std::memcpy(nv_ptr + 8, &next_slot, 2);

    if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);
    return true;
}

// ============================================================
// mark_dead — GC marks the line pointer DEAD.
// ============================================================

bool HeapFile::mark_dead(PageId page_id, SlotIdx slot_idx, u64 lsn) {
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    bool ok = page->mark_dead(slot_idx);
    if (ok) {
        if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
        pool_->mark_dirty(page_id);
    }
    pool_->unpin_page(page_id);
    return ok;
}

bool HeapFile::prune_obsolete_version(PageId page_id, SlotIdx slot_idx,
                                      u64 oldest_active_txn, u64 committed_xmax,
                                      u64 lsn) {
    if (committed_xmax == 0 || committed_xmax >= oldest_active_txn) {
        return false;
    }
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(slot_idx);
    if (!lp || !lp->is_valid() || lp->length < 26 ||
        static_cast<u32>(lp->offset) + 26 > kPageSize) {
        pool_->unpin_page(page_id);
        return false;
    }

    u64 xmax = 0;
    PageId next_page = kNullPageId;
    SlotIdx next_slot = kNullSlot;
    std::memcpy(&xmax, page->data() + lp->offset + 8, 8);
    std::memcpy(&next_page, page->data() + lp->offset + 16, 8);
    std::memcpy(&next_slot, page->data() + lp->offset + 24, 2);
    if (xmax != committed_xmax) {
        pool_->unpin_page(page_id);
        return false;
    }

    bool pruned = false;
    if (next_page == page_id && next_slot != kNullSlot) {
        pruned = page->redirect_slot(slot_idx, next_slot);
    } else {
        pruned = page->mark_dead(slot_idx);
    }
    if (pruned) {
        if (next_page != page_id) page->prune();
        if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
        pool_->mark_dirty(page_id);
    }
    pool_->unpin_page(page_id);
    return pruned;
}

// ============================================================
// rollback_insert — rollback: mark the slot UNUSED.
// ============================================================

bool HeapFile::rollback_insert(PageId page_id, SlotIdx slot_idx, u64 lsn) {
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(slot_idx);
    bool was_in_use = lp && lp->is_valid();
    bool ok = page->reclaim_slot(slot_idx);
    if (ok) {
        if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
        pool_->mark_dirty(page_id);
    }
    pool_->unpin_page(page_id);

    if (ok && was_in_use) {
        ensure_meta_loaded();
        if (meta_.num_tuples > 0) {
            meta_.num_tuples--;
        }
        note_meta_changed();
    }
    return ok;
}

bool HeapFile::rollback_delete(PageId page_id, SlotIdx slot_idx, u64 lsn) {
    LockGuard guard(latch_);
    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(slot_idx);
    if (!lp || !lp->is_valid()) {
        pool_->unpin_page(page_id);
        return false;
    }

    if (static_cast<u32>(lp->offset) + 26 > kPageSize || lp->length < 26) {
        pool_->unpin_page(page_id);
        return false;
    }

    byte* base = page->data() + lp->offset;
    u64 xmax = 0;
    PageId next_page = kNullPageId;
    SlotIdx next_slot = 0;
    std::memcpy(base + 8, &xmax, 8);
    std::memcpy(base + 16, &next_page, 8);
    std::memcpy(base + 24, &next_slot, 2);

    if (lsn != 0) pool_->set_page_lsn(page_id, lsn);
    pool_->mark_dirty(page_id);
    pool_->unpin_page(page_id);
    return true;
}

bool HeapFile::recover_insert_at(PageId page_id, SlotIdx slot_idx,
                                 const byte* data, u16 length, u64 lsn,
                                 bool* out_new_physical_tuple) {
    LockGuard guard(latch_);
    if (out_new_physical_tuple) *out_new_physical_tuple = false;

    auto result = pool_->fetch_page(page_id);
    if (!result.ok()) return false;

    Page* page = result.value();
    // Only initialize truly uninitialized pages (all-zero), not pages with mismatched IDs.
    // A page_id mismatch during recovery may indicate a valid page from another file,
    // and reinitializing it would destroy committed data.
    bool is_uninitialized = true;
    const byte* page_data = page->data();
    for (u32 i = 0; i < 64; i++) {
        if (page_data[i] != 0) { is_uninitialized = false; break; }
    }
    if (is_uninitialized) {
        page->init(page_id, PageType::kHeapData);
    }
    bool page_already_redone = !is_uninitialized && lsn != 0 && page->header()->lsn >= lsn;
    const LinePointer* lp = page->line_pointer(slot_idx);
    bool inserted = false;
    if (!page_already_redone && !(lp && lp->is_valid())) {
        if (page->insert_tuple_at(data, length, slot_idx) == kNullSlot) {
            pool_->unpin_page(page_id);
            return false;
        }
        inserted = true;
        if (out_new_physical_tuple) *out_new_physical_tuple = true;
    }
    if (!page_already_redone) {
        pool_->set_page_lsn(page_id, lsn);
        pool_->mark_dirty(page_id);
    }
    pool_->unpin_page(page_id);

    // The metadata update must always happen (even when an LSN match made us skip
    // the data insert), otherwise the page is orphaned across a recovery retry.
    ensure_meta_loaded();
    if (page_id > meta_.last_data_page_id) {
        // Link the previous last page to the new page.
        if (meta_.last_data_page_id != kNullPageId && meta_.last_data_page_id != page_id) {
            auto old_result = pool_->fetch_page(meta_.last_data_page_id);
            if (old_result.ok()) {
                Page* old_page = old_result.value();
                u64 next_ptr = page_id;
                std::memcpy(old_page->data() + kPageSize - sizeof(u64), &next_ptr, sizeof(u64));
                pool_->mark_dirty(meta_.last_data_page_id);
                pool_->unpin_page(meta_.last_data_page_id);
            }
        }
        meta_.last_data_page_id = page_id;
    }
    if (meta_.first_data_page_id == kNullPageId || page_id < meta_.first_data_page_id) {
        meta_.first_data_page_id = page_id;
    }
    u32 page_num = page_num_from_page(page_id);
    if (page_num > meta_.num_data_pages) meta_.num_data_pages = page_num;
    if (inserted) meta_.num_tuples++;
    note_meta_changed();
    return true;
}

bool HeapFile::recover_update(PageId old_page_id, SlotIdx old_slot_idx,
                              PageId new_page_id, SlotIdx new_slot_idx,
                              u64 xmax, const byte* data, u16 length, u64 lsn) {
    if (!recover_insert_at(new_page_id, new_slot_idx, data, length, lsn)) {
        return false;
    }
    bool ok1 = mark_deleted(old_page_id, old_slot_idx, xmax, lsn);
    bool ok2 = set_next_version(old_page_id, old_slot_idx, new_page_id, new_slot_idx, lsn);
    if (!ok1 || !ok2) {
        auto old_result = pool_->fetch_page(old_page_id);
        if (old_result.ok()) {
            Page* old_page = old_result.value();
            const LinePointer* old_lp = old_page->line_pointer(old_slot_idx);
            bool already_pruned = false;
            if (old_lp && old_lp->is_redirect()) {
                already_pruned = old_page->redirect_target(old_slot_idx) == new_slot_idx &&
                                  old_page_id == new_page_id;
            } else if (!old_lp || !old_lp->is_valid()) {
                already_pruned = old_page->header()->lsn >= lsn;
            }
            pool_->unpin_page(old_page_id);
            if (already_pruned) return true;
        }
        // Partial failure: roll back the insert by marking the new tuple as deleted
        rollback_insert(new_page_id, new_slot_idx, lsn);
        return false;
    }
    return true;
}

// ============================================================
// Metadata helpers.
// ============================================================

PageId HeapFile::first_data_page_id() const {
    ensure_meta_loaded();
    return meta_.first_data_page_id;
}

PageId HeapFile::meta_page_id() const {
    return make_page_id(table_id_, 0);
}

PageId HeapFile::allocate_new_page_id() {
    ensure_meta_loaded();
    u32 page_num = meta_.num_data_pages + 1;
    return make_page_id(table_id_, page_num);
}

void HeapFile::ensure_meta_loaded() const {
    if (!meta_loaded_) {
        const_cast<HeapFile*>(this)->load_meta();
    }
}

void HeapFile::load_meta() {
    PageId pid = meta_page_id();
    auto result = pool_->fetch_page(pid);
    if (result.ok()) {
        Page* page = result.value();
        std::memcpy(&meta_, page->data() + kPageHeaderSize, sizeof(HeapMeta));
        meta_loaded_ = true;
        meta_dirty_ = false;
        meta_mutations_since_save_ = 0;
        pool_->unpin_page(pid);
    }
}

void HeapFile::flush_meta() {
    if (meta_dirty_) {
        save_meta();
    }
}

void HeapFile::note_meta_changed() {
    meta_dirty_ = true;
    meta_mutations_since_save_++;
    if (meta_mutations_since_save_ >= 1024) {
        save_meta();
    }
}

void HeapFile::save_meta() {
    PageId pid = meta_page_id();
    auto result = pool_->fetch_page(pid);
    if (result.ok()) {
        Page* page = result.value();
        std::memcpy(page->data() + kPageHeaderSize, &meta_, sizeof(HeapMeta));
        meta_loaded_ = true;
        meta_dirty_ = false;
        meta_mutations_since_save_ = 0;
        pool_->mark_dirty(pid);
        pool_->unpin_page(pid);
    }
}

} // namespace minidb
