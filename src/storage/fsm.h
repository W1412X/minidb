/**
 * @file fsm.h
 * @brief Free Space Map — tracks per-page free space for efficient INSERT placement.
 *
 * PostgreSQL stores FSM as a separate relation fork with a tree structure.
 * MiniDB uses a simpler in-memory array: one byte per data page encoding
 * free-space in 32-byte categories (0 = empty … 255 = 8160+ bytes free).
 * The array is rebuilt on startup by scanning data pages.
 *
 * This avoids the "always append to last page" anti-pattern that wastes
 * space after DELETE/UPDATE and causes table bloat.
 */
#pragma once

#include "common/defs.h"
#include "container/vector.h"
#include "common/mutex.h"

namespace minidb {

class BufferPool;

/// Convert raw free bytes to an FSM category (0-255).
/// Category = min(free_bytes / 32, 255).
inline u8 fsm_encode(u16 free_bytes) {
    u32 cat = static_cast<u32>(free_bytes) / 32;
    return cat > 255 ? 255 : static_cast<u8>(cat);
}

/// Convert FSM category back to approximate free bytes.
inline u16 fsm_decode(u8 category) {
    return static_cast<u16>(static_cast<u32>(category) * 32);
}

/// Minimum category needed to store a tuple of the given size.
/// Accounts for LinePointer overhead (6 bytes).
inline u8 fsm_needed_category(u16 tuple_size) {
    u32 total = static_cast<u32>(tuple_size) + 6;  // tuple + LinePointer
    u32 cat = (total + 31) / 32;  // round up
    return cat > 255 ? 255 : static_cast<u8>(cat);
}

class FreeSpaceMap {
public:
    FreeSpaceMap() = default;

    /// Record the free space on a data page.
    void update(PageId page_id, u16 free_bytes);

    /// Find the first page with at least `needed` bytes free.
    /// Returns kNullPageId if none found.
    PageId find_page(u16 needed) const;

    /// Remove tracking for a page (e.g. after DROP TABLE).
    void remove(PageId page_id);

    /// Clear all entries.
    void clear();

    /// Number of tracked pages.
    u32 size() const;

    /// Rebuild FSM by scanning all data pages of a heap.
    void rebuild(BufferPool* pool, PageId first_data_page_id);

private:
    struct Entry {
        PageId page_id;
        u8 category;
        Entry() : page_id(kNullPageId), category(0) {}
        Entry(PageId pid, u8 cat) : page_id(pid), category(cat) {}
    };

    mutable Mutex latch_;
    Vector<Entry> entries_;
};

} // namespace minidb
