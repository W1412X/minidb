/**
 * @file shared_memory.h
 * @brief Shared memory management — mmap implementation, aligned with PostgreSQL Shared Memory concept
 *
 * PostgreSQL uses shared memory for:
 *   - Shared Buffers (Buffer pool)
 *   - Lock Table (Lock table)
 *   - Proc Array (Process array)
 *   - WAL Buffers
 *   - CLOG (transaction commit log)
 *
 * This module provides a basic shared-memory allocator usable by the above subsystems.
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "container/string.h"

namespace minidb {

// ============================================================
// Shared-memory block header.
// ============================================================

struct ShmBlockHeader {
    u32  size;       // block size (header included)
    u32  next;       // offset of the next block (0 = none)
    bool is_free;    // whether the block is free
    u8   reserved[3];
};

static constexpr u32 kShmBlockSize = sizeof(ShmBlockHeader);  // 16 bytes

// ============================================================
// Shared-memory region header.
// ============================================================

struct ShmRegionHeader {
    u64  magic;           // magic check
    u32  total_size;      // total size
    u32  used_size;       // bytes in use
    u32  free_list_head;  // offset of the free-list head
    u32  num_allocs;      // total allocations performed
    u32  num_frees;       // total frees performed
    u32  reserved;
};

static constexpr u64 kShmMagic = 0x53484D44424D494E;  // "SHMDBMIN"

// ============================================================
// SharedMemory — shared-memory allocator.
// ============================================================

class SharedMemory : NonCopyable {
public:
    // Create a new shared-memory region.
    static SharedMemory* create(const String& name, u32 size);

    // Attach to an existing shared-memory region.
    static SharedMemory* attach(const String& name);

    // Destroy the shared-memory region.
    static void destroy(const String& name);

    ~SharedMemory();

    // Allocate memory (returns offset, 0 on failure).
    u32 allocate(u32 size);

    // Free memory.
    void deallocate(u32 offset);

    // Resolve an offset to a pointer.
    void* get_ptr(u32 offset);

    // Get the region header.
    ShmRegionHeader* header() { return header_; }

    // Statistics.
    u32 total_size() const { return header_ ? header_->total_size : 0; }
    u32 used_size() const { return header_ ? header_->used_size : 0; }
    u32 free_size() const { return total_size() - used_size(); }

private:
    SharedMemory();

    String     name_;
    int        fd_;
    void*      mmap_ptr_;
    u32        mmap_size_;
    ShmRegionHeader* header_;
    byte*      data_;         // start of the data region
    RwLock     latch_;
};

} // namespace minidb
