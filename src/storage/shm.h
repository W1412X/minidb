/**
 * @file shm.h
 * @brief Shared memory management — mmap create/attach for multi-process sharing
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "record/schema.h"
#include "container/string.h"

namespace minidb {

// ============================================================
// Shared memory layout
// ============================================================

struct TxnSlotShm {
    u64 txn_id;           // 0 = free slot
    u64 snapshot_id;
    u64 commit_id;        // 0 = not committed
    u8  state;            // 0=ACTIVE, 1=COMMITTED, 2=ABORTED
};

struct TableSlotShm {
    u32 table_id;
    char name[64];
    u8  schema_buf[2048]; // serialised schema
    u32 schema_size;
    PageId first_page_id;
    u32 num_pages;
};

struct SharedMemoryHeader {
    // Metadata.
    u32 magic;                    // 0x4D444243 = "MDBC"
    u32 version;
    Mutex catalog_latch;
    Mutex txn_latch;

    // Global transaction id.
    u64 next_txn_id;

    // Transaction slots.
    static constexpr u32 kMaxTxns = 256;
    TxnSlotShm txn_slots[kMaxTxns];

    // Catalog
    static constexpr u32 kMaxTables = 128;
    TableSlotShm tables[kMaxTables];
    u32 table_count;
};

// ============================================================
// Shared-memory manager.
// ============================================================

class SharedMemory {
public:
    // Create a new shared-memory region.
    static SharedMemory* create(const String& name, u32 size);

    // Attach to an existing shared-memory region.
    static SharedMemory* attach(const String& name);

    // Destroy.
    static void destroy(const String& name);

    ~SharedMemory();

    SharedMemoryHeader* header() { return header_; }
    byte* data() { return data_; }
    u32 size() const { return size_; }

private:
    SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    int fd_;
    void* mmap_ptr_;
    u32 size_;
    SharedMemoryHeader* header_;
    byte* data_;
    String name_;
};

} // namespace minidb
