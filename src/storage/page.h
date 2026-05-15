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
// PageType
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
static constexpr u16 LP_NORMAL   = 1;  // 正在使用
static constexpr u16 LP_REDIRECT = 2;  // HOT redirect (reserved)
static constexpr u16 LP_DEAD     = 3;  // Dead, can be pruned/VACUUM cleaned

// ============================================================
// Page头 (24 bytes, 对齐 PostgreSQL PageHeaderData)
// ============================================================

#pragma pack(push, 1)
struct PageHeader {
    u64      page_id;            // Page全局唯一标识
    u64      lsn;                // 最后Modify该页的 WAL LSN
    u16      page_type;          // PageType
    u16      free_space_offset;  // 空闲空间起始偏移 (对齐 PG pd_lower)
    u16      num_tuples;         // Tuple计数 (包含 NORMAL + DEAD)
    u16      reserved;           // 保留字段
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == kPageHeaderSize,
              "PageHeader must be 24 bytes");

// ============================================================
// Row指针 (6 bytes, 对齐 PostgreSQL ItemIdData)
// ============================================================

#pragma pack(push, 1)
struct LinePointer {
    u16 offset;  // Tuple在页内的偏移
    u16 length;  // Tuple数据长度
    u16 flags;   // State标志 (LP_UNUSED/LP_NORMAL/LP_DEAD/LP_REDIRECT)

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
// Page 类 — 8KB 页的内存表示
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

    // --- 原始数据Access ---
    byte* data() { return data_; }
    const byte* data() const { return data_; }

    // --- Line Pointer 操作 ---
    LinePointer* line_pointer(u16 idx);
    const LinePointer* line_pointer(u16 idx) const;

    // --- 元组操作 ---
    // 返回 slot_idx, kNullSlot 表示空间不足
    SlotIdx insert_tuple(const byte* tuple_data, u16 length);
    SlotIdx insert_tuple_at(const byte* tuple_data, u16 length, SlotIdx target_slot);
    bool mark_dead(SlotIdx idx);     // 标记为 DEAD (MVCC 删除)
    bool reclaim_slot(SlotIdx idx);  // 物理回收 slot (GC/VACUUM)
    bool redirect_slot(SlotIdx idx, SlotIdx target_slot);
    SlotIdx redirect_target(SlotIdx idx) const;

    const byte* get_tuple_data(SlotIdx idx) const;
    u16 get_tuple_length(SlotIdx idx) const;

    // --- 页级修剪 ---
    // 回收所有 DEAD tuple 的空间, 返回回收的 slot 数
    u16 prune();

    // --- 空间管理 ---
    u16 get_free_space() const;
    bool has_enough_space(u16 tuple_size) const;

private:
    byte data_[kPageSize];
};

} // namespace minidb
