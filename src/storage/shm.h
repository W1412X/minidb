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
    u64 txn_id;           // 0 = 空闲
    u64 snapshot_id;
    u64 commit_id;        // 0 = 未提交
    u8  state;            // 0=ACTIVE, 1=COMMITTED, 2=ABORTED
};

struct TableSlotShm {
    u32 table_id;
    char name[64];
    u8  schema_buf[2048]; // Serialize后的 schema
    u32 schema_size;
    PageId first_page_id;
    u32 num_pages;
};

struct SharedMemoryHeader {
    // 元数据
    u32 magic;                    // 0x4D444243 = "MDBC"
    u32 version;
    Mutex catalog_latch;
    Mutex txn_latch;

    // 全局事务 ID
    u64 next_txn_id;

    // Transaction槽
    static constexpr u32 kMaxTxns = 256;
    TxnSlotShm txn_slots[kMaxTxns];

    // Catalog
    static constexpr u32 kMaxTables = 128;
    TableSlotShm tables[kMaxTables];
    u32 table_count;
};

// ============================================================
// SharedMemory 管理器
// ============================================================

class SharedMemory {
public:
    // Create新的Shared memory区域
    static SharedMemory* create(const String& name, u32 size);

    // 附加到已有Shared memory
    static SharedMemory* attach(const String& name);

    // 销毁
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
