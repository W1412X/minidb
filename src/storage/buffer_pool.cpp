/**
 * @file buffer_pool.cpp
 * @brief BufferPool implementation — Read-write lock optimization, disk I/O outside lock
 */
#include "storage/buffer_pool.h"
#include <cstdio>
#include "recovery/wal.h"
#include "container/utility.h"

namespace minidb {

// ============================================================
// Constructor / Destructor
// ============================================================

BufferPool::BufferPool(DiskManager* disk_mgr, u32 pool_size, u64 wait_timeout_ms,
                       u32 max_waiters, u32 partitions)
    : disk_mgr_(disk_mgr), wal_mgr_(nullptr), pool_size_(pool_size),
      partition_count_(partitions == 0 ? 1 : partitions),
      wait_timeout_ms_(wait_timeout_ms), max_waiters_(max_waiters) {
    if (partition_count_ > pool_size_) partition_count_ = pool_size_ == 0 ? 1 : pool_size_;
    frames_ = new Frame[pool_size];
    partitions_ = new BufferPoolPartition[partition_count_];
    for (u32 i = 0; i < pool_size; i++) {
        u32 part = i % partition_count_;
        frames_[i].partition_idx = part;
        frames_[i].lru_node = partitions_[part].lru_list.push_back_link(static_cast<FrameIdx>(i));
    }
}

BufferPool::~BufferPool() {
    flush_all();
    delete[] partitions_;
    delete[] frames_;
}

// ============================================================
// fetch_page — I/O outside write lock to reduce contention
// ============================================================

Result<Page*> BufferPool::fetch_page(PageId page_id, bool is_sequential) {
    if (page_id == kNullPageId) {
        return Status(ErrorCode::kInvalidArgument, "null page id");
    }

    while (true) {
        u32 part_idx = partition_for(page_id);
        BufferPoolPartition& partition = partitions_[part_idx];
        // Fast path: ReadGuard for cache hit (most common case)
        {
            ReadGuard guard(partition.latch);
            auto* frame_idx = find_page_mapping(partition, page_id);
            if (frame_idx) {
                Frame& f = frames_[*frame_idx];
                if (f.is_io_in_progress) {
                    // Another thread is already loading this page. Wait for that I/O
                    // rather than allocating a duplicate frame and doubling disk work.
                } else {
                    __sync_fetch_and_add(&f.pin_count, 1u);
                    record_hit();
                    // LRU update under read lock is not safe — defer to unpin or accept approximation
                    return &f.page;
                }
            }
        }
        record_miss();

        // Slow path: WriteGuard for cache miss (allocates victim, evicts, reads from disk)
        FrameIdx victim = kNullFrame;
        PageId evict_page_id = kNullPageId;
        bool need_wal_flush = false;
        bool should_wait = false;
        {
            WriteGuard guard(partition.latch);

            // Double-check: another thread may have loaded the page while we waited for write lock
            auto* frame_idx = find_page_mapping(partition, page_id);
            if (frame_idx) {
                Frame& f = frames_[*frame_idx];
                if (f.is_io_in_progress) {
                    should_wait = true;
                } else {
                    __sync_fetch_and_add(&f.pin_count, 1u);
                    move_to_lru_head(partition, *frame_idx);
                    record_hit();
                    return &f.page;
                }
            } else {
                // Miss, find victim frame
                victim = find_victim_frame(partition, part_idx);
                if (victim == kNullFrame) {
                    should_wait = true;
                } else {
                    // Evict old page — record metadata first, actual I/O outside lock
                    Frame& victim_frame = frames_[victim];
                    evict_page_id = victim_frame.page_id;
                    need_wal_flush = victim_frame.is_dirty && evict_page_id != kNullPageId &&
                                     wal_mgr_ && victim_frame.page.header()->lsn > wal_mgr_->durable_lsn();
                    victim_frame.pin_count = 1;
                    victim_frame.is_io_in_progress = true;
                    if (evict_page_id != kNullPageId) erase_page_mapping(partition, evict_page_id);
                    victim_frame.page_id = page_id;
                    insert_page_mapping(partition, page_id, victim);
                }
            }
        }

        if (should_wait) {
            if (!wait_for_buffer_slot()) {
                return Status(ErrorCode::kBufferFull, "buffer pool busy");
            }
            continue;
        }

        Frame& victim_frame = frames_[victim];
        // Phase 2: Disk I/O (lock-free)
        if (evict_page_id != kNullPageId) {
            if (need_wal_flush) wal_mgr_->flush();
            if (victim_frame.is_dirty) {
                disk_mgr_->write_page(evict_page_id, victim_frame.page.data());
            }
        }
        disk_mgr_->read_page(page_id, victim_frame.page.data());

        // Phase 3: Update metadata (holding write lock)
        {
            WriteGuard guard(partition.latch);
            victim_frame.page_id = page_id;
            victim_frame.pin_count = 1;
            victim_frame.is_dirty = false;
            victim_frame.is_io_in_progress = false;
            insert_page_mapping(partition, page_id, victim);

            if (is_sequential) {
                partition.lru_list.move_node_to_back(victim_frame.lru_node);
            } else {
                partition.lru_list.move_node_to_front(victim_frame.lru_node);
            }
        }
        notify_buffer_available();
        return &victim_frame.page;
    }
}

// ============================================================
// new_page — I/O outside write lock
// ============================================================

Result<Page*> BufferPool::new_page(PageId page_id, PageType type) {
    if (page_id == kNullPageId) {
        return Status(ErrorCode::kInvalidArgument, "null page id");
    }

    while (true) {
        u32 part_idx = partition_for(page_id);
        BufferPoolPartition& partition = partitions_[part_idx];
        FrameIdx victim = kNullFrame;
        PageId evict_page_id = kNullPageId;
        bool need_wal_flush = false;
        {
            WriteGuard guard(partition.latch);

            victim = find_victim_frame(partition, part_idx);
            if (victim == kNullFrame) {
                victim = kNullFrame;
            } else {
                Frame& victim_frame = frames_[victim];
                evict_page_id = victim_frame.page_id;
                need_wal_flush = victim_frame.is_dirty && evict_page_id != kNullPageId &&
                                 wal_mgr_ && victim_frame.page.header()->lsn > wal_mgr_->durable_lsn();
                victim_frame.pin_count = 1;
                victim_frame.is_io_in_progress = true;
                if (evict_page_id != kNullPageId) erase_page_mapping(partition, evict_page_id);
                victim_frame.page_id = page_id;
                insert_page_mapping(partition, page_id, victim);
            }
        }

        if (victim == kNullFrame) {
            if (!wait_for_buffer_slot()) {
                return Status(ErrorCode::kBufferFull, "buffer pool busy");
            }
            continue;
        }

        // Disk I/O (无锁)
        if (evict_page_id != kNullPageId) {
            if (need_wal_flush) wal_mgr_->flush();
            if (frames_[victim].is_dirty) {
                disk_mgr_->write_page(evict_page_id, frames_[victim].page.data());
            }
        }

        // Update元数据 (持有写锁)
        {
            WriteGuard guard(partition.latch);
            Frame& victim_frame = frames_[victim];
            victim_frame.page.init(page_id, type);
            victim_frame.page_id = page_id;
            victim_frame.pin_count = 1;
            victim_frame.is_dirty = true;
            victim_frame.is_io_in_progress = false;
            insert_page_mapping(partition, page_id, victim);
            partition.lru_list.move_node_to_front(victim_frame.lru_node);
        }
        notify_buffer_available();
        return &frames_[victim].page;
    }
}

// ============================================================
// unpin_page — 读锁即可
// ============================================================

void BufferPool::unpin_page(PageId page_id) {
    BufferPoolPartition& partition = partitions_[partition_for(page_id)];
    ReadGuard guard(partition.latch);

    auto* frame_idx = find_page_mapping(partition, page_id);
    if (!frame_idx) return;

    Frame& f = frames_[*frame_idx];
    // Atomic decrement: safe under ReadGuard
    u32 old = __sync_fetch_and_sub(&f.pin_count, 1u);
    if (old == 0) {
        // Already 0 — restore to 0 (underflow protection)
        __sync_fetch_and_add(&f.pin_count, 1u);
    } else if (old == 1) {
        notify_buffer_available();
    }
}

// ============================================================
// mark_dirty / set_page_lsn — 读锁即可
// ============================================================

void BufferPool::mark_dirty(PageId page_id) {
    BufferPoolPartition& partition = partitions_[partition_for(page_id)];
    WriteGuard guard(partition.latch);

    auto* frame_idx = find_page_mapping(partition, page_id);
    if (!frame_idx) return;

    frames_[*frame_idx].is_dirty = true;
}

void BufferPool::set_page_lsn(PageId page_id, u64 lsn) {
    BufferPoolPartition& partition = partitions_[partition_for(page_id)];
    WriteGuard guard(partition.latch);

    auto* frame_idx = find_page_mapping(partition, page_id);
    if (!frame_idx) return;

    // LSN 只能前进, 不能回退 (防止恢复 undo 操作降低已有的更高 LSN)
    u64 cur = frames_[*frame_idx].page.header()->lsn;
    if (lsn > cur) {
        frames_[*frame_idx].page.header()->lsn = lsn;
    }
    frames_[*frame_idx].is_dirty = true;
}

// ============================================================
// flush
// ============================================================

void BufferPool::flush_page(PageId page_id) {
    BufferPoolPartition& partition = partitions_[partition_for(page_id)];
    WriteGuard guard(partition.latch);

    auto* frame_idx = find_page_mapping(partition, page_id);
    if (!frame_idx) return;

    Frame& f = frames_[*frame_idx];
    if (f.is_dirty) {
        if (wal_mgr_ && f.page.header()->lsn > wal_mgr_->durable_lsn()) {
            wal_mgr_->flush();
        }
        disk_mgr_->write_page(f.page_id, f.page.data());
        f.is_dirty = false;
    }
}

void BufferPool::flush_all() {
    for (u32 p = 0; p < partition_count_; p++) {
        WriteGuard guard(partitions_[p].latch);
        for (u32 i = 0; i < pool_size_; i++) {
            if (frames_[i].partition_idx != p) continue;
            if (frames_[i].is_dirty && frames_[i].page_id != kNullPageId) {
                if (wal_mgr_ && frames_[i].page.header()->lsn > wal_mgr_->durable_lsn()) {
                    wal_mgr_->flush();
                }
                disk_mgr_->write_page(frames_[i].page_id, frames_[i].page.data());
                frames_[i].is_dirty = false;
            }
        }
    }
}

// ============================================================
// Internal methods
// ============================================================

FrameIdx BufferPool::find_victim_frame(BufferPoolPartition& partition, u32 partition_idx) {
    // 先找空帧
    for (u32 i = 0; i < pool_size_; i++) {
        if (frames_[i].partition_idx == partition_idx &&
            frames_[i].page_id == kNullPageId && !frames_[i].is_io_in_progress) {
            return i;
        }
    }

    // 从 LRU 尾部 (最久Unused) 开始扫描, 找 pin_count==0 的帧
    for (auto it = partition.lru_list.rbegin(); it != partition.lru_list.rend(); ++it) {
        FrameIdx idx = *it;
        if (frames_[idx].pin_count == 0 && !frames_[idx].is_io_in_progress) {
            return idx;
        }
    }

    return kNullFrame;
}

u32 BufferPool::partition_for(PageId page_id) const {
    return partition_count_ == 0 ? 0 : static_cast<u32>(page_id % partition_count_);
}

FrameIdx* BufferPool::find_page_mapping(BufferPoolPartition& partition, PageId page_id) {
    return partition.page_table.find(page_id);
}

void BufferPool::insert_page_mapping(BufferPoolPartition& partition, PageId page_id,
                                     FrameIdx frame_idx) {
    partition.page_table[page_id] = frame_idx;
}

void BufferPool::erase_page_mapping(BufferPoolPartition& partition, PageId page_id) {
    partition.page_table.erase(page_id);
}

void BufferPool::move_to_lru_head(BufferPoolPartition& partition, FrameIdx idx) {
    partition.lru_list.move_node_to_front(frames_[idx].lru_node);
}

bool BufferPool::wait_for_buffer_slot() {
    if (wait_timeout_ms_ == 0) {
        LockGuard guard(wait_latch_);
        stats_.wait_timeouts++;
        return false;
    }
    wait_latch_.lock();
    if (max_waiters_ != 0 && stats_.waiters >= max_waiters_) {
        stats_.wait_rejections++;
        wait_latch_.unlock();
        return false;
    }
    stats_.waiters++;
    bool ok = wait_cond_.timed_wait(wait_latch_, static_cast<u32>(wait_timeout_ms_));
    stats_.waiters--;
    if (!ok) stats_.wait_timeouts++;
    wait_latch_.unlock();
    return ok;
}

void BufferPool::notify_buffer_available() {
    LockGuard guard(wait_latch_);
    wait_cond_.broadcast();
}

void BufferPool::record_hit() {
    LockGuard guard(wait_latch_);
    stats_.hits++;
}

void BufferPool::record_miss() {
    LockGuard guard(wait_latch_);
    stats_.misses++;
}

BufferPoolStats BufferPool::stats() const {
    u32 dirty = 0;
    for (u32 p = 0; p < partition_count_; p++) {
        ReadGuard guard(partitions_[p].latch);
        for (u32 i = 0; i < pool_size_; i++) {
            if (frames_[i].partition_idx != p) continue;
            if (frames_[i].is_dirty && frames_[i].page_id != kNullPageId) dirty++;
        }
    }
    BufferPoolStats out;
    {
        LockGuard guard(wait_latch_);
        out = stats_;
    }
    out.dirty_pages = dirty;
    out.partitions = partition_count_;
    return out;
}

} // namespace minidb
