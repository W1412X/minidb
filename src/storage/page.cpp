/**
 * @file page.cpp
 * @brief Page class implementation — PostgreSQL style: LinePointer with flags, page-level pruning
 */
#include "storage/page.h"

namespace minidb {

// MAXALIGN: 8-byte alignment (PostgreSQL alignment rule)
static inline u16 max_align(u16 size) {
    return (size + 7) & ~7;
}

// Upper bound of valid data area (excluding page tail reserved space)
static constexpr u16 kDataUpperBound = kPageSize - kPageTailReserved;

void Page::init(PageId page_id, PageType type) {
    std::memset(data_, 0, kPageSize);
    PageHeader* hdr = header();
    hdr->page_id = page_id;
    hdr->lsn = kNullLsn;
    hdr->page_type = static_cast<u16>(type);
    hdr->free_space_offset = kPageHeaderSize;
    hdr->num_tuples = 0;
    hdr->reserved = 0;
}

// ============================================================
// Line Pointer
// ============================================================

LinePointer* Page::line_pointer(u16 idx) {
    u16 offset = kPageHeaderSize + idx * kLinePointerSize;
    if (offset + kLinePointerSize > kPageSize) return nullptr;
    return reinterpret_cast<LinePointer*>(data_ + offset);
}

const LinePointer* Page::line_pointer(u16 idx) const {
    u16 offset = kPageHeaderSize + idx * kLinePointerSize;
    if (offset + kLinePointerSize > kPageSize) return nullptr;
    return reinterpret_cast<const LinePointer*>(data_ + offset);
}

// ============================================================
// Tuple insert — prefer reusing DEAD slots, otherwise append
// ============================================================

SlotIdx Page::insert_tuple(const byte* tuple_data, u16 length) {
    PageHeader* hdr = header();
    u16 num = hdr->num_tuples;
    u16 aligned_len = max_align(length);
    SlotIdx reusable_slot = kNullSlot;

    // Strategy 1: reuse reclaimable slot
    for (u16 i = 0; i < num; i++) {
        LinePointer* lp = line_pointer(i);
        if (lp && lp->is_usable()) {
            if (lp->flags == LP_UNUSED && reusable_slot == kNullSlot) {
                reusable_slot = i;
            }
            // Check if enough space (original tuple may be smaller)
            // DEAD slot offset/length still retained, can be referenced
            if (lp->length > 0 && aligned_len <= max_align(lp->length)) {
                lp->mark_normal(lp->offset, length);
                std::memcpy(data_ + lp->offset, tuple_data, length);
                return i;
            }
        }
    }

    // Strategy 2: append to page end
    u16 lp_end = kPageHeaderSize + ((reusable_slot == kNullSlot) ? (num + 1) : num) * kLinePointerSize;

    // Find the smallest tuple offset (the tuple closest to the page start).
    u16 min_tuple_offset = kDataUpperBound;
    for (u16 i = 0; i < num; i++) {
        const LinePointer* existing = line_pointer(i);
        if (existing && existing->offset > 0 && existing->offset < min_tuple_offset) {
            min_tuple_offset = existing->offset;
        }
    }

    if (aligned_len > min_tuple_offset) return kNullSlot;
    u16 tuple_offset = min_tuple_offset - aligned_len;

    if (lp_end > tuple_offset) return kNullSlot;

    // Write the line pointer.
    SlotIdx target_slot = (reusable_slot == kNullSlot) ? num : reusable_slot;
    LinePointer* lp = line_pointer(target_slot);
    if (!lp) return kNullSlot;
    lp->mark_normal(tuple_offset, length);

    // Write the tuple bytes.
    std::memcpy(data_ + tuple_offset, tuple_data, length);

    if (reusable_slot == kNullSlot) {
        hdr->num_tuples = num + 1;
    }
    hdr->free_space_offset = lp_end;

    return target_slot;
}

SlotIdx Page::insert_tuple_at(const byte* tuple_data, u16 length, SlotIdx target_slot) {
    PageHeader* hdr = header();
    u16 num = hdr->num_tuples;
    u16 aligned_len = max_align(length);
    if (target_slot > num) return kNullSlot;
    if (target_slot < num) {
        LinePointer* existing = line_pointer(target_slot);
        if (existing && existing->is_valid()) return kNullSlot;
    }

    u16 lp_end = kPageHeaderSize + ((target_slot == num) ? (num + 1) : num) * kLinePointerSize;
    u16 min_tuple_offset = kDataUpperBound;
    for (u16 i = 0; i < num; i++) {
        const LinePointer* lp = line_pointer(i);
        if (lp && lp->offset > 0 && lp->offset < min_tuple_offset) {
            min_tuple_offset = lp->offset;
        }
    }
    if (aligned_len > min_tuple_offset) return kNullSlot;
    u16 tuple_offset = min_tuple_offset - aligned_len;
    if (lp_end > tuple_offset) return kNullSlot;

    LinePointer* lp = line_pointer(target_slot);
    if (!lp) return kNullSlot;
    lp->mark_normal(tuple_offset, length);
    std::memcpy(data_ + tuple_offset, tuple_data, length);
    if (target_slot == num) hdr->num_tuples = num + 1;
    hdr->free_space_offset = lp_end;
    return target_slot;
}

// ============================================================
// mark_dead — flag the slot DEAD (PostgreSQL style, slot stays in place).
// ============================================================

bool Page::mark_dead(SlotIdx idx) {
    PageHeader* hdr = header();
    if (idx >= hdr->num_tuples) return false;

    LinePointer* lp = line_pointer(idx);
    if (!lp || !lp->is_valid()) return false;

    lp->mark_dead();
    return true;
}

// ============================================================
// reclaim_slot — physically reclaim a slot (called by GC/VACUUM).
// ============================================================

bool Page::reclaim_slot(SlotIdx idx) {
    PageHeader* hdr = header();
    if (idx >= hdr->num_tuples) return false;

    LinePointer* lp = line_pointer(idx);
    if (!lp) return false;

    lp->mark_unused();
    return true;
}

bool Page::redirect_slot(SlotIdx idx, SlotIdx target_slot) {
    PageHeader* hdr = header();
    if (idx >= hdr->num_tuples || target_slot >= hdr->num_tuples) return false;
    LinePointer* lp = line_pointer(idx);
    if (!lp) return false;
    lp->mark_redirect(target_slot);
    return true;
}

SlotIdx Page::redirect_target(SlotIdx idx) const {
    const LinePointer* lp = line_pointer(idx);
    if (!lp || !lp->is_redirect()) return kNullSlot;
    return static_cast<SlotIdx>(lp->offset);
}

// ============================================================
// prune — page-level prune: reclaim every DEAD tuple's space.
//
// PostgreSQL-style: invoked implicitly during INSERT/UPDATE.
// Flags DEAD tuples' line pointers as UNUSED and
// marks their tuple-data area as reusable.
// ============================================================

u16 Page::prune() {
    PageHeader* hdr = header();
    u16 reclaimed = 0;

    // Early-out fast path: if there are no DEAD slots, the page already has
    // its tuple data area densely packed and there is nothing to compact.
    // The previous implementation unconditionally heap-allocated 8KB scratch
    // and did two memcpy passes over the whole page — every INSERT paid that
    // cost on the destination page, even when the page was freshly created.
    bool any_dead = false;
    for (u16 i = 0; i < hdr->num_tuples; i++) {
        LinePointer* lp = line_pointer(i);
        if (lp && lp->is_dead()) {
            any_dead = true;
            break;
        }
    }
    if (!any_dead) return 0;

    for (u16 i = 0; i < hdr->num_tuples; i++) {
        LinePointer* lp = line_pointer(i);
        if (lp && lp->is_dead()) {
            lp->mark_unused();
            reclaimed++;
        }
    }

    // Stack-allocated scratch instead of `new byte[kPageSize]` — avoids a
    // malloc/free pair per prune. kPageSize = 8KB which is well within the
    // pthread default stack size.
    byte tmp[kPageSize];
    u16 write_end = kDataUpperBound;
    for (u16 i = 0; i < hdr->num_tuples; i++) {
        const LinePointer* lp = line_pointer(i);
        if (!lp || !lp->is_valid()) continue;
        u16 aligned_len = max_align(lp->length);
        write_end -= aligned_len;
        std::memcpy(tmp + write_end, data_ + lp->offset, lp->length);
    }
    u16 cursor = kDataUpperBound;
    for (u16 i = 0; i < hdr->num_tuples; i++) {
        LinePointer* lp = line_pointer(i);
        if (!lp || !lp->is_valid()) continue;
        u16 aligned_len = max_align(lp->length);
        cursor -= aligned_len;
        std::memcpy(data_ + cursor, tmp + cursor, lp->length);
        lp->offset = cursor;
    }
    hdr->free_space_offset = kPageHeaderSize + hdr->num_tuples * kLinePointerSize;
    return reclaimed;
}

// ============================================================
// Tuple reads.
// ============================================================

const byte* Page::get_tuple_data(SlotIdx idx) const {
    const LinePointer* lp = line_pointer(idx);
    if (!lp || !lp->is_valid()) return nullptr;
    return data_ + lp->offset;
}

u16 Page::get_tuple_length(SlotIdx idx) const {
    const LinePointer* lp = line_pointer(idx);
    if (!lp || !lp->is_valid()) return 0;
    return lp->length;
}

// ============================================================
// Space management.
// ============================================================

u16 Page::get_free_space() const {
    const PageHeader* hdr = header();
    u16 lp_end = kPageHeaderSize + hdr->num_tuples * kLinePointerSize;

    u16 min_tuple_offset = kDataUpperBound;
    for (u16 i = 0; i < hdr->num_tuples; i++) {
        const LinePointer* lp = line_pointer(i);
        if (lp && lp->offset > 0 && lp->offset < min_tuple_offset) {
            min_tuple_offset = lp->offset;
        }
    }

    if (min_tuple_offset < lp_end) return 0;
    return min_tuple_offset - lp_end;
}

bool Page::has_enough_space(u16 tuple_size) const {
    const PageHeader* hdr = header();
    u16 aligned_len = max_align(tuple_size);

    bool has_unused_slot = false;
    // Check for reusable slots. DEAD slots can be reused in place; UNUSED slots still need contiguous free space.
    for (u16 i = 0; i < hdr->num_tuples; i++) {
        const LinePointer* lp = line_pointer(i);
        if (!lp || !lp->is_usable()) continue;
        if (lp->flags == LP_UNUSED) {
            has_unused_slot = true;
            continue;
        }
        if (lp->flags == LP_DEAD && aligned_len <= max_align(lp->length)) return true;
    }

    // Check whether there is room to append.
    u16 lp_end = kPageHeaderSize +
                 (has_unused_slot ? hdr->num_tuples : hdr->num_tuples + 1) * kLinePointerSize;
    u16 min_tuple_offset = kDataUpperBound;
    for (u16 i = 0; i < hdr->num_tuples; i++) {
        const LinePointer* lp = line_pointer(i);
        if (lp && lp->offset > 0 && lp->offset < min_tuple_offset) {
            min_tuple_offset = lp->offset;
        }
    }
    if (aligned_len > min_tuple_offset) return false;
    u16 tuple_offset = min_tuple_offset - aligned_len;
    return lp_end <= tuple_offset;
}

} // namespace minidb
