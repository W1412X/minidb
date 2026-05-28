/**
 * @file buffer_pool.cpp
 * @brief BufferPool implementation — Read-write lock optimization, disk I/O outside lock
 */
#include "storage/buffer_pool.h"
#include <cstdio>
#include "recovery/wal.h"
#include "container/utility.h"
#include "common/trace.h"

namespace minidb {

// ============================================================
// Constructor / Destructor
// ============================================================

BufferPool::BufferPool(PageStore* page_store, u32 pool_size, u64 wait_timeout_ms,
                       u32 max_waiters, u32 partitions, u32 flush_batch_size)
    : page_store_(page_store), wal_mgr_(nullptr), pool_size_(pool_size),
      partition_count_(partitions == 0 ? 1 : partitions),
      flush_batch_size_(flush_batch_size == 0 ? 1 : flush_batch_size),
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
    static thread_local PageId last_fetch_page_id = kNullPageId;
    bool sequential_hint = is_sequential ||
        (last_fetch_page_id != kNullPageId &&
         file_id_from_page(last_fetch_page_id) == file_id_from_page(page_id) &&
         page_num_from_page(last_fetch_page_id) + 1 == page_num_from_page(page_id));
    last_fetch_page_id = page_id;

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
                    f.pin_count.fetch_add(1u, std::memory_order_acquire);
                    record_hit();
                    if (TraceContext* trace = current_trace()) {
                        trace->record_buffer_fetch(page_id, true, sequential_hint);
                    }
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
                    f.pin_count.fetch_add(1u, std::memory_order_acquire);
                    move_to_lru_head(partition, *frame_idx);
                    record_hit();
                    if (TraceContext* trace = current_trace()) {
                        trace->record_buffer_fetch(page_id, true, sequential_hint);
                    }
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
                    victim_frame.pin_count.store(1, std::memory_order_release);
                    victim_frame.is_io_in_progress = true;
                    if (evict_page_id != kNullPageId) erase_page_mapping(partition, evict_page_id);
                    victim_frame.page_id = page_id;
                    insert_page_mapping(partition, page_id, victim);
                    if (evict_page_id != kNullPageId) {
                        if (TraceContext* trace = current_trace()) {
                            trace->record_buffer_eviction(evict_page_id, page_id,
                                                          victim_frame.is_dirty.load(std::memory_order_relaxed));
                        }
                    }
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
            if (victim_frame.is_dirty) {
                if (need_wal_flush && !flush_frame_wal_first(victim_frame)) {
                    WriteGuard guard(partition.latch);
                    restore_evicted_frame(partition, victim, evict_page_id);
                    notify_buffer_available();
                    return Status(ErrorCode::kIOError, "failed to flush WAL before dirty page eviction");
                }
                Result<void> write_result = page_store_->write_page(
                    evict_page_id, victim_frame.page.data(), victim_frame.page.header()->lsn);
                if (!write_result.ok()) {
                    WriteGuard guard(partition.latch);
                    restore_evicted_frame(partition, victim, evict_page_id);
                    notify_buffer_available();
                    return write_result.error();
                }
            }
        }
        Result<void> read_result = page_store_->read_page(page_id, victim_frame.page.data());
        if (!read_result.ok()) {
            WriteGuard guard(partition.latch);
            erase_page_mapping(partition, page_id);
            victim_frame.page_id = kNullPageId;
            victim_frame.pin_count.store(0, std::memory_order_release);
            victim_frame.is_dirty = false;
            victim_frame.is_io_in_progress = false;
            notify_buffer_available();
            return read_result.error();
        }

        // Phase 3: Update metadata (holding write lock)
        {
            WriteGuard guard(partition.latch);
            victim_frame.page_id = page_id;
            victim_frame.pin_count.store(1, std::memory_order_release);
            victim_frame.is_dirty = false;
            victim_frame.is_io_in_progress = false;
            insert_page_mapping(partition, page_id, victim);

            if (sequential_hint) {
                partition.lru_list.move_node_to_back(victim_frame.lru_node);
            } else {
                partition.lru_list.move_node_to_front(victim_frame.lru_node);
            }
        }
        notify_buffer_available();
        if (TraceContext* trace = current_trace()) {
            trace->record_buffer_fetch(page_id, false, sequential_hint);
        }
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
        bool should_wait = false;
        {
            WriteGuard guard(partition.latch);

            auto* existing = find_page_mapping(partition, page_id);
            if (existing) {
                Frame& f = frames_[*existing];
                if (f.is_io_in_progress) {
                    should_wait = true;
                } else {
                    f.pin_count.fetch_add(1u, std::memory_order_acquire);
                    move_to_lru_head(partition, *existing);
                    return &f.page;
                }
            } else {
                victim = find_victim_frame(partition, part_idx);
                if (victim == kNullFrame) {
                    should_wait = true;
                } else {
                    Frame& victim_frame = frames_[victim];
                    evict_page_id = victim_frame.page_id;
                    need_wal_flush = victim_frame.is_dirty && evict_page_id != kNullPageId &&
                                     wal_mgr_ && victim_frame.page.header()->lsn > wal_mgr_->durable_lsn();
                    victim_frame.pin_count.store(1, std::memory_order_release);
                    victim_frame.is_io_in_progress = true;
                    if (evict_page_id != kNullPageId) erase_page_mapping(partition, evict_page_id);
                    victim_frame.page_id = page_id;
                    insert_page_mapping(partition, page_id, victim);
                    if (evict_page_id != kNullPageId) {
                        if (TraceContext* trace = current_trace()) {
                            trace->record_buffer_eviction(evict_page_id, page_id,
                                                          victim_frame.is_dirty.load(std::memory_order_relaxed));
                        }
                    }
                }
            }
        }

        if (should_wait || victim == kNullFrame) {
            if (!wait_for_buffer_slot()) {
                return Status(ErrorCode::kBufferFull, "buffer pool busy");
            }
            continue;
        }

        // Disk I/O (lock-free).
        if (evict_page_id != kNullPageId) {
            if (frames_[victim].is_dirty) {
                if (need_wal_flush && !flush_frame_wal_first(frames_[victim])) {
                    WriteGuard guard(partition.latch);
                    restore_evicted_frame(partition, victim, evict_page_id);
                    notify_buffer_available();
                    return Status(ErrorCode::kIOError, "failed to flush WAL before dirty page eviction");
                }
                Result<void> write_result = page_store_->write_page(
                    evict_page_id, frames_[victim].page.data(), frames_[victim].page.header()->lsn);
                if (!write_result.ok()) {
                    WriteGuard guard(partition.latch);
                    restore_evicted_frame(partition, victim, evict_page_id);
                    notify_buffer_available();
                    return write_result.error();
                }
            }
        }

        // Update metadata (still holding the write lock).
        {
            WriteGuard guard(partition.latch);
            Frame& victim_frame = frames_[victim];
            victim_frame.page.init(page_id, type);
            victim_frame.page_id = page_id;
            victim_frame.pin_count.store(1, std::memory_order_release);
            victim_frame.is_dirty = true;
            victim_frame.is_io_in_progress = false;
            insert_page_mapping(partition, page_id, victim);
            partition.lru_list.move_node_to_front(victim_frame.lru_node);
        }
        notify_buffer_available();
        if (TraceContext* trace = current_trace()) {
            trace->record_buffer_new_page(page_id);
        }
        return &frames_[victim].page;
    }
}

// ============================================================
// unpin_page — read lock is enough.
// ============================================================

void BufferPool::unpin_page(PageId page_id) {
    BufferPoolPartition& partition = partitions_[partition_for(page_id)];
    ReadGuard guard(partition.latch);

    auto* frame_idx = find_page_mapping(partition, page_id);
    if (!frame_idx) return;

    Frame& f = frames_[*frame_idx];
    // Atomic decrement: safe under ReadGuard
    u32 old = f.pin_count.load(std::memory_order_acquire);
    while (old != 0 && !f.pin_count.compare_exchange_weak(old, old - 1,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {}
    if (old == 1) {
        notify_buffer_available();
    }
}

// ============================================================
// mark_dirty / set_page_lsn — read lock is enough.
// ============================================================

void BufferPool::mark_dirty(PageId page_id) {
    BufferPoolPartition& partition = partitions_[partition_for(page_id)];
    // ReadGuard is sufficient: is_dirty is now an atomic<bool>, and the
    // page-table lookup is safe under a read lock. The previous WriteGuard
    // serialized every page mutation across the partition — a major
    // contention point in INSERT/UPDATE-heavy workloads.
    ReadGuard guard(partition.latch);

    auto* frame_idx = find_page_mapping(partition, page_id);
    if (!frame_idx) return;

    frames_[*frame_idx].is_dirty.store(true, std::memory_order_release);
    if (TraceContext* trace = current_trace()) {
        trace->record_buffer_dirty(page_id);
    }
}

void BufferPool::set_page_lsn(PageId page_id, u64 lsn) {
    BufferPoolPartition& partition = partitions_[partition_for(page_id)];
    // ReadGuard suffices — the caller already pins the frame so it cannot
    // be evicted, and is_dirty is atomic. The page-header LSN update races
    // with concurrent writers, but we always advance monotonically (CAS
    // would be theoretically cleaner; in practice this field is only
    // touched while the page's logical lock is held by the writer).
    ReadGuard guard(partition.latch);

    auto* frame_idx = find_page_mapping(partition, page_id);
    if (!frame_idx) return;

    u64 cur = frames_[*frame_idx].page.header()->lsn;
    if (lsn > cur) {
        frames_[*frame_idx].page.header()->lsn = lsn;
    }
    frames_[*frame_idx].is_dirty.store(true, std::memory_order_release);
    if (TraceContext* trace = current_trace()) {
        trace->record_buffer_dirty(page_id);
    }
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
        if (!flush_frame_wal_first(f)) return;
        Result<void> write_result = page_store_->write_page(f.page_id, f.page.data(),
                                                            f.page.header()->lsn);
        if (write_result.ok()) {
            f.is_dirty = false;
            if (TraceContext* trace = current_trace()) {
                trace->record_buffer_flush(page_id);
            }
        }
    }
}

void BufferPool::flush_all() {
    Vector<PageWriteRequest> batch;
    Vector<FrameIdx> batch_frames;
    batch.reserve(flush_batch_size_);
    batch_frames.reserve(flush_batch_size_);
    for (u32 p = 0; p < partition_count_; p++) {
        WriteGuard guard(partitions_[p].latch);
        for (u32 i = 0; i < pool_size_; i++) {
            if (frames_[i].partition_idx != p) continue;
            if (frames_[i].is_dirty && frames_[i].page_id != kNullPageId) {
                if (!flush_frame_wal_first(frames_[i])) continue;
                batch.push_back(PageWriteRequest(frames_[i].page_id,
                                                 frames_[i].page.data(),
                                                 frames_[i].page.header()->lsn));
                batch_frames.push_back(static_cast<FrameIdx>(i));
                if (batch.size() >= flush_batch_size_) {
                    Vector<PageIOResult> results = page_store_->write_pages(batch);
                    for (u32 r = 0; r < results.size() && r < batch_frames.size(); r++) {
                        if (results[r].ok()) {
                            frames_[batch_frames[r]].is_dirty = false;
                            if (TraceContext* trace = current_trace()) {
                                trace->record_buffer_flush(frames_[batch_frames[r]].page_id);
                            }
                        }
                    }
                    batch.clear();
                    batch_frames.clear();
                }
            }
        }
        if (!batch.empty()) {
            Vector<PageIOResult> results = page_store_->write_pages(batch);
            for (u32 r = 0; r < results.size() && r < batch_frames.size(); r++) {
                if (results[r].ok()) {
                    frames_[batch_frames[r]].is_dirty = false;
                    if (TraceContext* trace = current_trace()) {
                        trace->record_buffer_flush(frames_[batch_frames[r]].page_id);
                    }
                }
            }
            batch.clear();
            batch_frames.clear();
        }
    }
}

// ============================================================
// Internal methods
// ============================================================

FrameIdx BufferPool::find_victim_frame(BufferPoolPartition& partition, u32 partition_idx) {
    // Two-pass scan: prefer empty frames (no eviction work) over evictable
    // ones. Both passes walk the partition's LRU list rather than the global
    // frame array — the previous global scan touched all pool_size_ frames
    // even when only one partition was relevant. Empty frames are preferred
    // because evicting an unpinned resident page costs a disk write if the
    // page is dirty, and sequential-scan policy intentionally parks freshly
    // loaded pages at the LRU tail to be evicted first — we don't want to
    // throw them away when an empty frame is sitting elsewhere in the list.
    (void)partition_idx;
    FrameIdx evictable = kNullFrame;
    for (auto it = partition.lru_list.rbegin(); it != partition.lru_list.rend(); ++it) {
        FrameIdx idx = *it;
        Frame& f = frames_[idx];
        if (f.is_io_in_progress) continue;
        if (f.page_id == kNullPageId) return idx;             // empty — best choice
        if (evictable == kNullFrame &&
            f.pin_count.load(std::memory_order_acquire) == 0) {
            evictable = idx;                                   // remember but keep looking
        }
    }
    return evictable;
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
        counters_.wait_timeouts.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    wait_latch_.lock();
    u32 cur_waiters = counters_.waiters.load(std::memory_order_relaxed);
    if (max_waiters_ != 0 && cur_waiters >= max_waiters_) {
        counters_.wait_rejections.fetch_add(1, std::memory_order_relaxed);
        wait_latch_.unlock();
        return false;
    }
    counters_.waiters.fetch_add(1, std::memory_order_relaxed);
    bool ok = wait_cond_.timed_wait(wait_latch_, static_cast<u32>(wait_timeout_ms_));
    counters_.waiters.fetch_sub(1, std::memory_order_relaxed);
    if (!ok) counters_.wait_timeouts.fetch_add(1, std::memory_order_relaxed);
    wait_latch_.unlock();
    return ok;
}

void BufferPool::notify_buffer_available() {
    LockGuard guard(wait_latch_);
    wait_cond_.broadcast();
}

void BufferPool::record_hit() {
    // Lock-free counter: relaxed ordering is sufficient because the counter
    // value is observational, not used for synchronisation. The previous code
    // took a global mutex for every cache hit, which serialized the entire
    // buffer pool on read-heavy workloads.
    counters_.hits.fetch_add(1, std::memory_order_relaxed);
}

void BufferPool::record_miss() {
    counters_.misses.fetch_add(1, std::memory_order_relaxed);
}

bool BufferPool::flush_frame_wal_first(Frame& frame) {
    if (!wal_mgr_) return true;
    u64 page_lsn = frame.page.header()->lsn;
    if (page_lsn > wal_mgr_->durable_lsn() && !wal_mgr_->flush_until(page_lsn)) {
        return false;
    }
    page_store_->set_durable_lsn(wal_mgr_->durable_lsn());
    return wal_mgr_->durable_lsn() >= page_lsn;
}

void BufferPool::restore_evicted_frame(BufferPoolPartition& partition, FrameIdx victim,
                                       PageId old_page_id) {
    Frame& frame = frames_[victim];
    erase_page_mapping(partition, frame.page_id);
    frame.page_id = old_page_id;
    frame.pin_count.store(0, std::memory_order_release);
    frame.is_io_in_progress = false;
    if (old_page_id != kNullPageId) insert_page_mapping(partition, old_page_id, victim);
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
    out.hits = counters_.hits.load(std::memory_order_relaxed);
    out.misses = counters_.misses.load(std::memory_order_relaxed);
    out.waiters = counters_.waiters.load(std::memory_order_relaxed);
    out.wait_timeouts = counters_.wait_timeouts.load(std::memory_order_relaxed);
    out.wait_rejections = counters_.wait_rejections.load(std::memory_order_relaxed);
    out.dirty_pages = dirty;
    out.partitions = partition_count_;
    return out;
}

} // namespace minidb
