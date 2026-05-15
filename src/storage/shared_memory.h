/**
 * @file shared_memory.h
 * @brief Shared memory management — mmap implementation, aligned with PostgreSQL Shared Memory concept
 *
 * PostgreSQL 的Shared memory用于:
 *   - Shared Buffers (Buffer pool)
 *   - Lock Table (Lock table)
 *   - Proc Array (Process array)
 *   - WAL Buffers (WAL 缓冲)
 *   - CLOG (事务提交日志)
 *
 * 本实现提供基础的Shared memory分配器, 可用于上述各种子系统。
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "container/string.h"

namespace minidb {

// ============================================================
// Shared memory块头
// ============================================================

struct ShmBlockHeader {
    u32  size;       // 块大小 (含头)
    u32  next;       // 下一个块的偏移 (0 = 无)
    bool is_free;    // 是否空闲
    u8   reserved[3];
};

static constexpr u32 kShmBlockSize = sizeof(ShmBlockHeader);  // 16 bytes

// ============================================================
// Shared memory区域头
// ============================================================

struct ShmRegionHeader {
    u64  magic;           // 校验值
    u32  total_size;      // 总大小
    u32  used_size;       // 已使用大小
    u32  free_list_head;  // 空闲Linked List头偏移
    u32  num_allocs;      // 分配次数
    u32  num_frees;       // Free次数
    u32  reserved;
};

static constexpr u64 kShmMagic = 0x53484D44424D494E;  // "SHMDBMIN"

// ============================================================
// SharedMemory — Shared memory分配器
// ============================================================

class SharedMemory : NonCopyable {
public:
    // Create新的Shared memory区域
    static SharedMemory* create(const String& name, u32 size);

    // 附加到已有Shared memory区域
    static SharedMemory* attach(const String& name);

    // 销毁Shared memory区域
    static void destroy(const String& name);

    ~SharedMemory();

    // 分配内存 (返回偏移, 失败返回 0)
    u32 allocate(u32 size);

    // Free内存
    void deallocate(u32 offset);

    // 通过偏移Get指针
    void* get_ptr(u32 offset);

    // Get区域头
    ShmRegionHeader* header() { return header_; }

    // 统计信息
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
    byte*      data_;         // 数据区起始
    RwLock     latch_;
};

} // namespace minidb
