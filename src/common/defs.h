/**
 * @file defs.h
 * @brief Basic type definitions
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace minidb {

// 基础Integer type aliases
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Byte type
using byte = uint8_t;

// Page面相关
using PageId   = u64;
using LSN      = u64;
using SlotIdx  = u16;
using FrameIdx = u32;

// Transaction related
using TxnId = u64;

// 无效值常量
constexpr PageId   kNullPageId  = 0;
constexpr LSN      kNullLsn     = 0;
constexpr TxnId    kNullTxnId   = 0;
constexpr SlotIdx  kNullSlot    = 0xFFFF;
constexpr FrameIdx kNullFrame   = 0xFFFFFFFF;

// PageId 编解码: 高32位=file_id, 低32位=page_num
inline u32 file_id_from_page(PageId pid) {
    return static_cast<u32>(pid >> 32);
}

inline u32 page_num_from_page(PageId pid) {
    return static_cast<u32>(pid & 0xFFFFFFFF);
}

inline PageId make_page_id(u32 file_id, u32 page_num) {
    return (static_cast<PageId>(file_id) << 32) | page_num;
}

} // namespace minidb
