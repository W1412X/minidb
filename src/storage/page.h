/**
 * @file page.h
 * @brief 8KB page structure — aligned with PostgreSQL PageHeaderData layout
 *
 * Page is the basic I/O unit of the storage engine; all data is read/written to disk in pages.
 * Layout: [PageHeader 24B] [LinePointers 6B each] [FreeSpace] [TupleData]
 */
#pragma once

#include "common/defs.h"
#include "common/config.h"
#include <cstring>

namespace minidb {

// ============================================================
// PageType — high-level page kind.
// ============================================================

enum class PageType : u16 {
    kHeapData    = 1,
    kHeapMeta    = 2,
    kIndexData   = 3,
    kIndexMeta   = 4,
    kFreeList    = 5,
    kWalPage     = 6,
    kCatalogData = 7,
};

// ============================================================
// LinePointer flags (aligned with PostgreSQL LP_NORMAL/LP_DEAD/LP_REDIRECT)
// ============================================================

static constexpr u16 LP_UNUSED   = 0;  // Unused
static constexpr u16 LP_NORMAL   = 1;  // in use
static constexpr u16 LP_REDIRECT = 2;  // HOT redirect (reserved)
static constexpr u16 LP_DEAD     = 3;  // Dead, can be pruned/VACUUM cleaned

// ============================================================
// Page header (24 bytes, mirrors PostgreSQL PageHeaderData layout).
// ============================================================

#pragma pack(push, 1)
struct PageHeader {
    u64      page_id;            // global page id
    u64      lsn;                // WAL LSN of last modification to this page
    u16      page_type;          // PageType discriminator
    u16      free_space_offset;  // free-space start offset (PostgreSQL pd_lower)
    u16      num_tuples;         // tuple count (includes NORMAL + DEAD)
    u16      reserved;           // reserved (also holds page checksum when enabled)
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == kPageHeaderSize,
              "PageHeader must be 24 bytes");

// ============================================================
// Line pointer (6 bytes, mirrors PostgreSQL ItemIdData).
// ============================================================

#pragma pack(push, 1)
struct LinePointer {
    u16 offset;  // tuple offset within the page
    u16 length;  // tuple data length
    u16 flags;   // state flags (LP_UNUSED / LP_NORMAL / LP_DEAD / LP_REDIRECT)

    bool is_valid() const { return flags == LP_NORMAL && offset > 0 && length > 0; }
    bool is_dead() const { return flags == LP_DEAD; }
    bool is_redirect() const { return flags == LP_REDIRECT; }
    bool is_usable() const { return flags == LP_UNUSED || flags == LP_DEAD; }

    void mark_normal(u16 off, u16 len) {
        offset = off; length = len; flags = LP_NORMAL;
    }
    void mark_dead() { flags = LP_DEAD; }
    void mark_redirect(SlotIdx target_slot) {
        offset = target_slot; length = 0; flags = LP_REDIRECT;
    }
    void mark_unused() { offset = 0; length = 0; flags = LP_UNUSED; }
};
#pragma pack(pop)

static_assert(sizeof(LinePointer) == 6, "LinePointer must be 6 bytes");

// ============================================================
// Page — in-memory view of an 8 KB on-disk page.
// ============================================================

class Page {
public:
    Page() { std::memset(data_, 0, kPageSize); }

    // Initialize new page
    void init(PageId page_id, PageType type);

    // --- Header Access ---
    PageHeader* header() {
        return reinterpret_cast<PageHeader*>(data_);
    }
    const PageHeader* header() const {
        return reinterpret_cast<const PageHeader*>(data_);
    }

    // --- Raw data access ---
    byte* data() { return data_; }
    const byte* data() const { return data_; }

    // --- Line pointer operations ---
    LinePointer* line_pointer(u16 idx);
    const LinePointer* line_pointer(u16 idx) const;

    // --- Tuple operations ---
    // Returns slot_idx, or kNullSlot when there is no room.
    SlotIdx insert_tuple(const byte* tuple_data, u16 length);
    SlotIdx insert_tuple_at(const byte* tuple_data, u16 length, SlotIdx target_slot);
    bool mark_dead(SlotIdx idx);     // mark DEAD (MVCC delete)
    bool reclaim_slot(SlotIdx idx);  // physically reclaim a slot (GC/VACUUM)
    bool redirect_slot(SlotIdx idx, SlotIdx target_slot);
    SlotIdx redirect_target(SlotIdx idx) const;

    const byte* get_tuple_data(SlotIdx idx) const;
    u16 get_tuple_length(SlotIdx idx) const;

    // --- Page-level prune ---
    // Reclaim every DEAD tuple's space, returns the number of slots reclaimed.
    u16 prune();

    // --- Space management ---
    u16 get_free_space() const;
    bool has_enough_space(u16 tuple_size) const;

private:
    byte data_[kPageSize];
};

} // namespace minidb
