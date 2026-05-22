/**
 * @file visibility_map.cpp
 * @brief Visibility Map implementation.
 */
#include "storage/visibility_map.h"

namespace minidb {

VisibilityMap::Entry* VisibilityMap::find_entry(PageId page_id) const {
    for (u32 i = 0; i < entries_.size(); i++) {
        if (entries_[i].page_id == page_id) {
            return &entries_[i];
        }
    }
    return nullptr;
}

void VisibilityMap::set_visible(PageId page_id) {
    LockGuard guard(latch_);
    Entry* e = find_entry(page_id);
    if (e) {
        e->flags |= kVisible;
    } else {
        entries_.push_back(Entry(page_id, kVisible));
    }
}

void VisibilityMap::set_frozen(PageId page_id) {
    LockGuard guard(latch_);
    Entry* e = find_entry(page_id);
    if (e) {
        e->flags |= (kVisible | kFrozen);
    } else {
        entries_.push_back(Entry(page_id, kVisible | kFrozen));
    }
}

void VisibilityMap::clear_page(PageId page_id) {
    LockGuard guard(latch_);
    Entry* e = find_entry(page_id);
    if (e) e->flags = 0;
}

bool VisibilityMap::is_visible(PageId page_id) const {
    LockGuard guard(latch_);
    const Entry* e = find_entry(page_id);
    return e && (e->flags & kVisible);
}

bool VisibilityMap::is_frozen(PageId page_id) const {
    LockGuard guard(latch_);
    const Entry* e = find_entry(page_id);
    return e && (e->flags & kFrozen);
}

void VisibilityMap::remove(PageId page_id) {
    LockGuard guard(latch_);
    for (u32 i = 0; i < entries_.size(); i++) {
        if (entries_[i].page_id == page_id) {
            entries_[i] = entries_.back();
            entries_.pop_back();
            return;
        }
    }
}

void VisibilityMap::clear() {
    LockGuard guard(latch_);
    entries_.clear();
}

u32 VisibilityMap::size() const {
    LockGuard guard(latch_);
    return entries_.size();
}

void VisibilityMap::stats(u32* visible_count, u32* frozen_count) const {
    LockGuard guard(latch_);
    u32 v = 0, f = 0;
    for (u32 i = 0; i < entries_.size(); i++) {
        if (entries_[i].flags & kVisible) v++;
        if (entries_[i].flags & kFrozen) f++;
    }
    if (visible_count) *visible_count = v;
    if (frozen_count) *frozen_count = f;
}

} // namespace minidb
