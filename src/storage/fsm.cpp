/**
 * @file fsm.cpp
 * @brief Free Space Map implementation.
 */
#include "storage/fsm.h"
#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace minidb {

void FreeSpaceMap::update(PageId page_id, u16 free_bytes) {
    u8 cat = fsm_encode(free_bytes);
    LockGuard guard(latch_);
    for (u32 i = 0; i < entries_.size(); i++) {
        if (entries_[i].page_id == page_id) {
            entries_[i].category = cat;
            return;
        }
    }
    entries_.push_back(Entry(page_id, cat));
}

PageId FreeSpaceMap::find_page(u16 needed) const {
    u8 min_cat = fsm_needed_category(needed);
    LockGuard guard(latch_);
    for (u32 i = 0; i < entries_.size(); i++) {
        if (entries_[i].category >= min_cat) {
            return entries_[i].page_id;
        }
    }
    return kNullPageId;
}

void FreeSpaceMap::remove(PageId page_id) {
    LockGuard guard(latch_);
    for (u32 i = 0; i < entries_.size(); i++) {
        if (entries_[i].page_id == page_id) {
            entries_[i] = entries_.back();
            entries_.pop_back();
            return;
        }
    }
}

void FreeSpaceMap::clear() {
    LockGuard guard(latch_);
    entries_.clear();
}

u32 FreeSpaceMap::size() const {
    LockGuard guard(latch_);
    return entries_.size();
}

void FreeSpaceMap::rebuild(BufferPool* pool, PageId first_data_page_id) {
    LockGuard guard(latch_);
    entries_.clear();
    if (!pool || first_data_page_id == kNullPageId) return;

    PageId pid = first_data_page_id;
    while (pid != kNullPageId) {
        auto result = pool->fetch_page(pid);
        if (!result.ok()) break;
        Page* page = result.value();
        u16 free = page->get_free_space();
        entries_.push_back(Entry(pid, fsm_encode(free)));
        // Follow linked-list to next data page.
        // next_page_id is stored at offset kPageSize - 8 in data pages.
        PageId next = kNullPageId;
        const byte* raw = page->data();
        std::memcpy(&next, raw + kPageSize - sizeof(PageId), sizeof(PageId));
        pool->unpin_page(pid);
        pid = next;
    }
}

} // namespace minidb
