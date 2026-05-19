/**
 * @file buffer_pool.h
 * @brief Buffer pool — LRU replacement + anti-pollution, aligned with PostgreSQL Shared Buffers concept
 *
 * Manages disk page cache in memory, provides fetch_page/unpin/mark_dirty/flush interfaces.
 * Uses LRU replacement strategy; supports is_sequential parameter to prevent full table scans from polluting hot data.
 */
#pragma once

#include "common/defs.h"
#include "common/config.h"
#include "common/mutex.h"
#include "common/status.h"
#include "container/hash_map.h"
#include "container/linked_list.h"
#include "storage/page.h"
#include "storage/page_store.h"
#include <atomic>

namespace minidb {

class WalManager;

struct BufferPoolStats {
    u64 hits = 0;
    u64 misses = 0;
    u32 waiters = 0;
    u64 wait_timeouts = 0;
    u64 wait_rejections = 0;
    u32 dirty_pages = 0;
    u32 partitions = 1;
};

struct BufferPoolPartition {
    HashMap<PageId, FrameIdx> page_table;
    LinkedList<FrameIdx> lru_list;
    mutable RwLock latch;
};

// Buffer pool frame
struct Frame {
    Page    page;
    PageId  page_id;
    std::atomic<u32> pin_count;
    bool    is_dirty;
    bool    is_io_in_progress;
    u32     partition_idx;
    LinkedList<FrameIdx>::Node* lru_node;

    Frame() : page_id(kNullPageId), pin_count(0), is_dirty(false),
              is_io_in_progress(false), partition_idx(0), lru_node(nullptr) {}
};

class BufferPool : NonCopyable {
public:
    BufferPool(PageStore* page_store, u32 pool_size, u64 wait_timeout_ms = 5000,
               u32 max_waiters = 1024, u32 partitions = 1,
               u32 flush_batch_size = 64);
    ~BufferPool();

    // Get page
    // is_sequential: true=sequential scan (new page at LRU tail), false=random access (new page at head)
    Result<Page*> fetch_page(PageId page_id, bool is_sequential = false);

    // Create new blank page
    Result<Page*> new_page(PageId page_id, PageType type = PageType::kHeapData);

    // 减少 pin_count
    void unpin_page(PageId page_id);

    // 标记脏页
    void mark_dirty(PageId page_id);
    void set_page_lsn(PageId page_id, u64 lsn);
    void set_wal_manager(WalManager* wal_mgr) { wal_mgr_ = wal_mgr; }

    // 刷盘
    void flush_page(PageId page_id);
    void flush_all();

    // State查询
    u32 pool_size() const { return pool_size_; }
    BufferPoolStats stats() const;

private:
    // Search可驱逐的帧
    FrameIdx find_victim_frame(BufferPoolPartition& partition, u32 partition_idx);

    u32 partition_for(PageId page_id) const;
    FrameIdx* find_page_mapping(BufferPoolPartition& partition, PageId page_id);
    void insert_page_mapping(BufferPoolPartition& partition, PageId page_id, FrameIdx frame_idx);
    void erase_page_mapping(BufferPoolPartition& partition, PageId page_id);
    void move_to_lru_head(BufferPoolPartition& partition, FrameIdx idx);
    bool wait_for_buffer_slot();
    void notify_buffer_available();
    void record_hit();
    void record_miss();
    bool flush_frame_wal_first(Frame& frame);
    void restore_evicted_frame(BufferPoolPartition& partition, FrameIdx victim,
                               PageId old_page_id);

    PageStore* page_store_;
    WalManager* wal_mgr_;
    u32 pool_size_;
    u32 partition_count_;
    u32 flush_batch_size_;
    u64 wait_timeout_ms_;
    u32 max_waiters_;
    Frame* frames_;                    // 固定大小帧Array
    BufferPoolPartition* partitions_;
    mutable Mutex wait_latch_;
    CondVar wait_cond_;
    BufferPoolStats stats_;
};

} // namespace minidb
