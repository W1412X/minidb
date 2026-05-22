/**
 * @file visibility_map.h
 * @brief Visibility Map — tracks which pages are "all-visible" and "all-frozen".
 *
 * PostgreSQL's VM uses 2 bits per page (all-visible + all-frozen).
 * MiniDB follows the same design:
 *   - all-visible:  all live tuples on the page are visible to every active snapshot.
 *                   Enables index-only scans (skip heap fetch) and vacuum skip.
 *   - all-frozen:   all live tuples on the page have been frozen (xmin replaced
 *                   with FrozenTransactionId).  Subset of all-visible.
 *
 * The VM is rebuilt on startup by scanning data pages, similar to FSM.
 * It is invalidated per-page whenever a DML modifies the page.
 */
#pragma once

#include "common/defs.h"
#include "container/vector.h"
#include "common/mutex.h"

namespace minidb {

class VisibilityMap {
public:
    VisibilityMap() = default;

    /// Mark a page as all-visible (all live tuples visible to every snapshot).
    void set_visible(PageId page_id);

    /// Mark a page as all-frozen (all live tuples frozen).
    /// Implies all-visible.
    void set_frozen(PageId page_id);

    /// Clear visibility flags for a page (e.g. after INSERT/UPDATE/DELETE).
    void clear_page(PageId page_id);

    /// Check if a page is all-visible.
    bool is_visible(PageId page_id) const;

    /// Check if a page is all-frozen.
    bool is_frozen(PageId page_id) const;

    /// Remove a page from the VM entirely (e.g. DROP TABLE).
    void remove(PageId page_id);

    /// Clear all entries.
    void clear();

    /// Number of tracked pages.
    u32 size() const;

    /// Statistics: how many pages are all-visible, all-frozen.
    void stats(u32* visible_count, u32* frozen_count) const;

private:
    static constexpr u8 kVisible = 0x01;
    static constexpr u8 kFrozen  = 0x02;

    struct Entry {
        PageId page_id;
        u8 flags;  // kVisible | kFrozen
        Entry() : page_id(kNullPageId), flags(0) {}
        Entry(PageId pid, u8 f) : page_id(pid), flags(f) {}
    };

    Entry* find_entry(PageId page_id) const;

    mutable Mutex latch_;
    mutable Vector<Entry> entries_;
};

} // namespace minidb
