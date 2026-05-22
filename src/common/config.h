/**
 * @file config.h
 * @brief Global configuration constants
 */
#pragma once

#include "defs.h"

namespace minidb {

// === Storage configuration ===
constexpr u32 kPageSize        = 8192;
constexpr u32 kPageHeaderSize  = 24;
constexpr u32 kLinePointerSize = 6;  // offset(2) + length(2) + flags(2)
constexpr u32 kPageTailReserved = 8;  // bytes reserved at the page tail (next_page_id)

// === Buffer poolConfig ===
constexpr u32 kDefaultPoolFrames = 256;
constexpr u32 kMaxPoolFrames     = 65536;

// === B+ TreeConfig ===
constexpr u32 kBTreeOrder     = 128;
constexpr u32 kMaxKeysPerNode = kBTreeOrder - 1;
constexpr u32 kMaxChildren    = kBTreeOrder;

// === WAL Config ===
constexpr u32 kWalBufferSize    = kPageSize;
constexpr u32 kMaxLogRecordSize = 256;

// === Network configuration ===
constexpr u16 kDefaultPort    = 5433;
constexpr u32 kMaxConnections = 64;

// === Cache Line ===
constexpr u32 kCacheLineSize = 64;

// === Protection threshold (prevent loops/abnormal chains) ===
constexpr u32 kMaxPageChainHops = 1'000'000;

} // namespace minidb
