#include "recovery/wal.h"
#include "database/database.h"
#include "storage/page.h"
#include "storage/heap_file.h"
#include "catalog/catalog.h"
#include "container/vector.h"
#include "container/hash_map.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>

namespace minidb {

static u32 value_to_wal_bytes(const Value& key, byte* out, u32 cap) {
    if (!out || cap < 1) return 0;
    u32 size = key.serialized_size();
    if (size > cap) return 0;
    key.serialize(out);
    return size;
}

WalManager::WalManager(const String& wal_dir)
    : WalManager(wal_dir, 64ULL * 1024ULL * 1024ULL, true) {}

WalManager::WalManager(const String& wal_dir, u64 segment_size_bytes, bool fsync_enabled)
    : WalManager(wal_dir, segment_size_bytes, fsync_enabled, false, 0) {}

WalManager::WalManager(const String& wal_dir, u64 segment_size_bytes, bool fsync_enabled,
                       bool group_commit_enabled, u64 group_commit_delay_ms)
    : wal_dir_(wal_dir), fd_(-1), next_lsn_(1), durable_lsn_(0), last_written_lsn_(0),
      segment_size_bytes_(segment_size_bytes == 0 ? 64ULL * 1024ULL * 1024ULL : segment_size_bytes),
      fsync_enabled_(fsync_enabled), group_commit_enabled_(group_commit_enabled),
      group_commit_delay_ms_(group_commit_delay_ms), pending_commit_waiters_(0),
      group_commit_batches_(0), commit_batch_id_(0),
      buffer_flushes_(0), buffered_bytes_(0),
      bytes_since_checkpoint_(0), write_buf_pos_(0) {
    mkdir(wal_dir.c_str(), 0755);
    String path = wal_dir + "/wal.log";
    fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
}

WalManager::~WalManager() {
    if (fd_ >= 0) {
        (void)flush_buffer();
        if (fsync_enabled_) fsync(fd_);
        close(fd_);
    }
}

bool WalManager::write_direct(const byte* data, u32 len) {
    if (fd_ < 0 || !data) return false;
    const byte* ptr = data;
    u32 remaining = len;
    while (remaining > 0) {
        ssize_t written = ::write(fd_, ptr, remaining);
        if (written <= 0) return false;
        ptr += written;
        remaining -= static_cast<u32>(written);
    }
    return true;
}

bool WalManager::append_to_buffer(const byte* data, u32 len) {
    if (len == 0) return true;
    if (!data) return false;
    if (len > kWalBufferSize) {
        if (!flush_buffer()) return false;
        return write_direct(data, len);
    }
    if (write_buf_pos_ + len > kWalBufferSize) {
        if (!flush_buffer()) return false;
    }
    std::memcpy(write_buf_ + write_buf_pos_, data, len);
    write_buf_pos_ += len;
    buffered_bytes_ += len;
    if (write_buf_pos_ == kWalBufferSize && !flush_buffer()) return false;
    return true;
}

bool WalManager::flush_buffer() {
    if (write_buf_pos_ == 0) return true;
    if (write_direct(write_buf_, write_buf_pos_)) {
        buffer_flushes_++;
        write_buf_pos_ = 0;
        return true;
    }
    return false;
}

u64 WalManager::write_record(WalType type, u64 txn_id, const byte* data, u32 data_len) {
    LockGuard guard(latch_);
    if (fd_ < 0) return 0;

    WalRecord hdr;
    hdr.lsn = next_lsn_;
    hdr.txn_id = txn_id;
    hdr.type = type;
    hdr.data_len = data_len;

    if (!append_to_buffer(reinterpret_cast<const byte*>(&hdr), sizeof(hdr))) {
        return 0;
    }
    if (data_len > 0 && !append_to_buffer(data, data_len)) return 0;

    bytes_since_checkpoint_ += sizeof(hdr) + data_len;
    next_lsn_++;
    last_written_lsn_ = hdr.lsn;

    struct stat st;
    if (fstat(fd_, &st) == 0 &&
        static_cast<u64>(st.st_size) + write_buf_pos_ > segment_size_bytes_) {
        if (flush_buffer() && (!fsync_enabled_ || fsync(fd_) == 0)) {
            durable_lsn_ = last_written_lsn_;
        }
    }
    return hdr.lsn;
}

u64 WalManager::log_begin(u64 txn_id) {
    return write_record(WalType::kTxnBegin, txn_id, nullptr, 0);
}

u64 WalManager::log_commit(u64 txn_id) {
    u64 lsn = write_record(WalType::kTxnCommit, txn_id, nullptr, 0);
    if (lsn == 0) return 0;
    // Caller must observe durability before exposing the commit. If fsync
    // fails or group-commit closes without making us durable, return 0 so
    // the transaction layer can run the abort path.
    if (!flush_commit(lsn)) return 0;
    return lsn;
}

u64 WalManager::log_abort(u64 txn_id) {
    u64 lsn = write_record(WalType::kTxnAbort, txn_id, nullptr, 0);
    if (lsn != 0) {
        LockGuard guard(latch_);
        if (flush_buffer() && fd_ >= 0 && (!fsync_enabled_ || fsync(fd_) == 0)) durable_lsn_ = lsn;
    }
    return lsn;
}

u64 WalManager::log_insert(u64 txn_id, u32 table_id, PageId page_id, SlotIdx slot_idx,
                           const byte* data, u16 size) {
    struct __attribute__((packed)) {
        u32 table_id;
        u64 page_id;
        u16 slot_idx;
        u16 data_size;
    } buf;
    buf.table_id = table_id;
    buf.page_id = page_id;
    buf.slot_idx = slot_idx;
    buf.data_size = size;

    u32 total = sizeof(buf) + size;
    byte* full = static_cast<byte*>(std::malloc(total));
    if (!full) return 0;
    std::memcpy(full, &buf, sizeof(buf));
    if (size > 0) std::memcpy(full + sizeof(buf), data, size);

    u64 lsn = write_record(WalType::kInsert, txn_id, full, total);
    std::free(full);
    return lsn;
}

u64 WalManager::log_delete(u64 txn_id, u32 table_id, PageId page_id, SlotIdx slot_idx) {
    struct __attribute__((packed)) {
        u32 table_id;
        u64 page_id;
        u16 slot_idx;
    } buf;
    buf.table_id = table_id;
    buf.page_id = page_id;
    buf.slot_idx = slot_idx;

    return write_record(WalType::kDelete, txn_id, reinterpret_cast<byte*>(&buf), sizeof(buf));
}

u64 WalManager::log_update(u64 txn_id, u32 table_id,
                           PageId old_page_id, SlotIdx old_slot_idx,
                           PageId new_page_id, SlotIdx new_slot_idx,
                           const byte* new_data, u16 size) {
    struct __attribute__((packed)) {
        u32 table_id;
        u64 old_page_id;
        u16 old_slot_idx;
        u64 new_page_id;
        u16 new_slot_idx;
        u16 data_size;
    } buf;
    buf.table_id = table_id;
    buf.old_page_id = old_page_id;
    buf.old_slot_idx = old_slot_idx;
    buf.new_page_id = new_page_id;
    buf.new_slot_idx = new_slot_idx;
    buf.data_size = size;

    u32 total = sizeof(buf) + size;
    byte* full = static_cast<byte*>(std::malloc(total));
    if (!full) return 0;
    std::memcpy(full, &buf, sizeof(buf));
    if (size > 0) std::memcpy(full + sizeof(buf), new_data, size);

    u64 lsn = write_record(WalType::kUpdate, txn_id, full, total);
    std::free(full);
    return lsn;
}

u64 WalManager::log_index_insert(u64 txn_id, u32 index_id, const Value& key, const RecordId& rid) {
    u32 key_size = key.serialized_size();
    if (key_size == 0 || key_size > 0xffff) return 0;
    std::vector<byte> key_buf(key_size);
    if (value_to_wal_bytes(key, key_buf.data(), key_size) != key_size) return 0;
    struct __attribute__((packed)) {
        u32 index_id;
        u64 page_id;
        u16 slot_idx;
        u16 key_size;
    } hdr;
    hdr.index_id = index_id;
    hdr.page_id = rid.page_id;
    hdr.slot_idx = rid.slot_idx;
    hdr.key_size = static_cast<u16>(key_size);
    u32 total = sizeof(hdr) + key_size;
    byte* full = static_cast<byte*>(std::malloc(total));
    if (!full) return 0;
    std::memcpy(full, &hdr, sizeof(hdr));
    std::memcpy(full + sizeof(hdr), key_buf.data(), key_size);
    u64 lsn = write_record(WalType::kIndexInsert, txn_id, full, total);
    std::free(full);
    return lsn;
}

u64 WalManager::log_index_delete(u64 txn_id, u32 index_id, const Value& key, const RecordId& rid) {
    u32 key_size = key.serialized_size();
    if (key_size == 0 || key_size > 0xffff) return 0;
    std::vector<byte> key_buf(key_size);
    if (value_to_wal_bytes(key, key_buf.data(), key_size) != key_size) return 0;
    struct __attribute__((packed)) {
        u32 index_id;
        u64 page_id;
        u16 slot_idx;
        u16 key_size;
    } hdr;
    hdr.index_id = index_id;
    hdr.page_id = rid.page_id;
    hdr.slot_idx = rid.slot_idx;
    hdr.key_size = static_cast<u16>(key_size);
    u32 total = sizeof(hdr) + key_size;
    byte* full = static_cast<byte*>(std::malloc(total));
    if (!full) return 0;
    std::memcpy(full, &hdr, sizeof(hdr));
    std::memcpy(full + sizeof(hdr), key_buf.data(), key_size);
    u64 lsn = write_record(WalType::kIndexDelete, txn_id, full, total);
    std::free(full);
    return lsn;
}

u64 WalManager::checkpoint(CheckpointPageFlush flush_pages_cb, void* ctx) {
    LockGuard guard(latch_);
    if (fd_ < 0) return 0;

    // Phase 1: write the kCheckpoint marker. Done inline (rather than via
    // write_record) because we must hold the latch from now until the
    // log is truncated, so no other writer can sneak in records that
    // would later get clobbered by the truncate.
    WalRecord hdr;
    hdr.lsn = next_lsn_;
    hdr.txn_id = 0;
    hdr.type = WalType::kCheckpoint;
    hdr.data_len = 0;
    if (!append_to_buffer(reinterpret_cast<const byte*>(&hdr), sizeof(hdr))) return 0;
    bytes_since_checkpoint_ += sizeof(hdr);
    next_lsn_++;
    last_written_lsn_ = hdr.lsn;
    u64 lsn = hdr.lsn;

    // Phase 2: fsync the WAL up to and including the kCheckpoint record.
    if (!flush_buffer()) return 0;
    if (fsync_enabled_ && fsync(fd_) != 0) return 0;
    durable_lsn_ = last_written_lsn_;
    commit_cond_.broadcast();

    // Phase 3: flush dirty pages while the latch is still held. At this
    // point every dirty page satisfies page_lsn <= durable_lsn_, so
    // BufferPool::flush_frame_wal_first takes its fast path and does NOT
    // try to re-acquire the WAL latch — no deadlock. New writers are
    // parked on the WAL latch and cannot dirty additional pages until
    // we release it, so the dirty-page set is stable for the entire
    // flush.
    if (flush_pages_cb) flush_pages_cb(ctx);

    // Phase 4: truncate the WAL. Safe now because everything that
    // referenced records we are about to discard is durable on disk.
    if (fd_ >= 0) {
        u64 keep_next = next_lsn_;
        u64 keep_durable = durable_lsn_;
        close(fd_);
        String path = wal_dir_ + "/wal.log";
        fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        next_lsn_ = keep_next;
        durable_lsn_ = keep_durable;
        last_written_lsn_ = keep_durable;
    }

    bytes_since_checkpoint_ = 0;
    return lsn;
}

bool WalManager::flush_commit(u64 lsn) {
    LockGuard guard(latch_);
    if (fd_ < 0) return false;

    if (!fsync_enabled_) {
        if (!flush_buffer()) return false;
        if (durable_lsn_ < lsn) durable_lsn_ = lsn;
        commit_cond_.broadcast();
        return durable_lsn_ >= lsn;
    }

    if (!group_commit_enabled_ || group_commit_delay_ms_ == 0) {
        if (!flush_buffer()) return false;
        if (fsync(fd_) != 0) return false;
        durable_lsn_ = last_written_lsn_;
        commit_cond_.broadcast();
        return durable_lsn_ >= lsn;
    }

    pending_commit_waiters_++;
    const bool leader = (pending_commit_waiters_ == 1);
    if (leader) {
        commit_cond_.timed_wait(latch_, static_cast<u32>(group_commit_delay_ms_));
        const bool ok = flush_buffer() && fsync(fd_) == 0;
        if (ok) durable_lsn_ = last_written_lsn_;
        group_commit_batches_++;
        commit_batch_id_++;
        pending_commit_waiters_ = 0;
        commit_cond_.broadcast();
        return ok && durable_lsn_ >= lsn;
    }

    // Follower: wait for our LSN to become durable, or for the current
    // batch to close (so we can observe a failed flush). Loop on the
    // predicate to stay correct under spurious wake-ups.
    const u64 entry_batch = commit_batch_id_;
    while (durable_lsn_ < lsn && commit_batch_id_ == entry_batch) {
        commit_cond_.wait(latch_);
    }
    return durable_lsn_ >= lsn;
}

void WalManager::flush() {
    LockGuard guard(latch_);
    if (fd_ >= 0) {
        if (flush_buffer() && (!fsync_enabled_ || fsync(fd_) == 0)) {
            durable_lsn_ = last_written_lsn_;
            commit_cond_.broadcast();
        }
    }
}

bool WalManager::flush_until(u64 lsn) {
    LockGuard guard(latch_);
    if (lsn == 0 || durable_lsn_ >= lsn) return true;
    if (fd_ < 0) return false;
    if (!flush_buffer()) return false;
    if (fsync_enabled_ && fsync(fd_) != 0) return false;
    durable_lsn_ = last_written_lsn_;
    commit_cond_.broadcast();
    return durable_lsn_ >= lsn;
}

void WalManager::ensure_next_lsn_at_least(u64 lsn) {
    LockGuard guard(latch_);
    if (lsn == 0) return;
    if (next_lsn_ <= lsn) next_lsn_ = lsn + 1;
    if (last_written_lsn_ < lsn) last_written_lsn_ = lsn;
    if (durable_lsn_ < lsn) durable_lsn_ = lsn;
}

void WalManager::truncate() {
    LockGuard guard(latch_);
    if (fd_ >= 0) {
        u64 keep_next = next_lsn_;
        u64 keep_durable = durable_lsn_;
        close(fd_);
        String path = wal_dir_ + "/wal.log";
        fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        next_lsn_ = keep_next;
        durable_lsn_ = keep_durable;
        last_written_lsn_ = keep_durable;
    }
}

bool WalManager::recover(Database* db) {
    String path = wal_dir_ + "/wal.log";
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct ReplayRef {
        WalRecord hdr;
        off_t data_offset;
    };
    Vector<ReplayRef> undo_refs;
    HashMap<u64, bool> committed;
    HashMap<u64, bool> aborted;
    u64 max_lsn = 0;
    u64 max_txn_id = 0;

    auto read_exact = [](int read_fd, void* out, u32 len) -> bool {
        byte* ptr = static_cast<byte*>(out);
        u32 remaining = len;
        while (remaining > 0) {
            ssize_t n = ::read(read_fd, ptr, remaining);
            if (n <= 0) return false;
            ptr += n;
            remaining -= static_cast<u32>(n);
        }
        return true;
    };

    auto skip_bytes = [](int read_fd, u32 len) -> bool {
        return lseek(read_fd, static_cast<off_t>(len), SEEK_CUR) >= 0;
    };

    constexpr u32 kMaxReplayDataLen = kPageSize + 256;

    auto page_lsn_at = [db](PageId page_id, LSN* out) -> bool {
        if (!db || !out || page_id == kNullPageId) return false;
        auto res = db->pool().fetch_page(page_id);
        if (!res.ok()) return false;
        Page* page = res.value();
        *out = page->header()->lsn;
        db->pool().unpin_page(page_id);
        return true;
    };

    WalRecord hdr;
    while (read_exact(fd, &hdr, sizeof(hdr))) {
        max_lsn = hdr.lsn;
        if (hdr.txn_id > max_txn_id) max_txn_id = hdr.txn_id;
        if (hdr.data_len > kMaxReplayDataLen || !skip_bytes(fd, hdr.data_len)) break;
        if (hdr.type == WalType::kTxnCommit) {
            committed[hdr.txn_id] = true;
        } else if (hdr.type == WalType::kTxnAbort) {
            aborted[hdr.txn_id] = true;
        }
    }
    bool needs_index_rebuild = false;

    if (db) {
        if (lseek(fd, 0, SEEK_SET) < 0) {
            close(fd);
            return false;
        }

        while (read_exact(fd, &hdr, sizeof(hdr))) {
            if (hdr.data_len > kMaxReplayDataLen) break;
            off_t data_offset = lseek(fd, 0, SEEK_CUR);
            bool is_data_record = hdr.type == WalType::kInsert ||
                                  hdr.type == WalType::kDelete ||
                                  hdr.type == WalType::kUpdate;
            if (!is_data_record) {
                if (!skip_bytes(fd, hdr.data_len)) break;
                continue;
            }

            Vector<byte> data;
            data.resize(hdr.data_len);
            if (hdr.data_len > 0 && !read_exact(fd, data.data(), hdr.data_len)) break;
            const bool* is_committed = committed.find(hdr.txn_id);
            if (!(is_committed && *is_committed)) {
                ReplayRef ref;
                ref.hdr = hdr;
                ref.data_offset = data_offset;
                undo_refs.push_back(ref);
                continue;
            }

            if (hdr.type == WalType::kInsert) {
                struct __attribute__((packed)) InsertHdr {
                    u32 table_id;
                    u64 page_id;
                    u16 slot_idx;
                    u16 data_size;
                };
                if (data.size() < sizeof(InsertHdr)) continue;
                InsertHdr ih;
                std::memcpy(&ih, data.data(), sizeof(ih));
                if (data.size() < sizeof(ih) + ih.data_size) continue;
                HeapFile* heap = db->get_heap_file(ih.table_id);
                if (heap) {
                    bool new_tuple = false;
                    heap->recover_insert_at(ih.page_id, ih.slot_idx,
                                            data.data() + sizeof(ih),
                                            ih.data_size, hdr.lsn, &new_tuple);
                    if (new_tuple) needs_index_rebuild = true;
                }
            } else if (hdr.type == WalType::kDelete) {
                struct __attribute__((packed)) DeleteHdr {
                    u32 table_id;
                    u64 page_id;
                    u16 slot_idx;
                };
                if (data.size() < sizeof(DeleteHdr)) continue;
                DeleteHdr dh;
                std::memcpy(&dh, data.data(), sizeof(dh));
                HeapFile* heap = db->get_heap_file(dh.table_id);
                if (heap) {
                    LSN page_lsn = 0;
                    if (page_lsn_at(dh.page_id, &page_lsn) && page_lsn >= hdr.lsn) {
                        continue;
                    }
                    if (heap->mark_deleted(dh.page_id, dh.slot_idx, hdr.txn_id, hdr.lsn)) {
                        needs_index_rebuild = true;
                    }
                }
            } else if (hdr.type == WalType::kUpdate) {
                struct __attribute__((packed)) UpdateHdr {
                    u32 table_id;
                    u64 old_page_id;
                    u16 old_slot_idx;
                    u64 new_page_id;
                    u16 new_slot_idx;
                    u16 data_size;
                };
                if (data.size() < sizeof(UpdateHdr)) continue;
                UpdateHdr uh;
                std::memcpy(&uh, data.data(), sizeof(uh));
                if (data.size() < sizeof(uh) + uh.data_size) continue;
                HeapFile* heap = db->get_heap_file(uh.table_id);
                if (heap) {
                    LSN old_lsn = 0;
                    LSN new_lsn = 0;
                    bool old_done = page_lsn_at(uh.old_page_id, &old_lsn) && old_lsn >= hdr.lsn;
                    bool new_done = page_lsn_at(uh.new_page_id, &new_lsn) && new_lsn >= hdr.lsn;
                    if (old_done && new_done) {
                        continue;
                    }
                    bool applied = false;
                    bool inserted_new = false;
                    if (!new_done) {
                        if (!heap->recover_insert_at(uh.new_page_id, uh.new_slot_idx,
                                                     data.data() + sizeof(uh),
                                                     uh.data_size, hdr.lsn,
                                                     &inserted_new)) {
                            continue;
                        }
                        applied = true;
                    }
                    if (!old_done) {
                        bool marked = heap->mark_deleted(uh.old_page_id, uh.old_slot_idx,
                                                         hdr.txn_id, hdr.lsn);
                        bool linked = heap->set_next_version(uh.old_page_id, uh.old_slot_idx,
                                                             uh.new_page_id, uh.new_slot_idx,
                                                             hdr.lsn);
                        if (!marked || !linked) {
                            if (inserted_new) {
                                heap->rollback_insert(uh.new_page_id, uh.new_slot_idx, hdr.lsn);
                            }
                            continue;
                        }
                        applied = true;
                    }
                    if (applied) {
                        needs_index_rebuild = true;
                    }
                }
            }
        }

        for (i32 i = static_cast<i32>(undo_refs.size()) - 1; i >= 0; i--) {
            const ReplayRef& ref = undo_refs[static_cast<u32>(i)];
            Vector<byte> data;
            data.resize(ref.hdr.data_len);
            if (lseek(fd, ref.data_offset, SEEK_SET) < 0) continue;
            if (ref.hdr.data_len > 0 && !read_exact(fd, data.data(), ref.hdr.data_len)) continue;

            if (ref.hdr.type == WalType::kInsert) {
                struct __attribute__((packed)) InsertHdr {
                    u32 table_id;
                    u64 page_id;
                    u16 slot_idx;
                    u16 data_size;
                };
                if (data.size() < sizeof(InsertHdr)) continue;
                InsertHdr ih;
                std::memcpy(&ih, data.data(), sizeof(ih));
                HeapFile* heap = db->get_heap_file(ih.table_id);
                if (heap) {
                    // Safety check: only rollback if the slot's xmin still matches
                    // the rolled-back transaction. If a committed INSERT reused the
                    // slot, the xmin will be different and we must NOT roll it back.
                    auto pg_res = db->pool().fetch_page(ih.page_id);
                    if (pg_res.ok()) {
                        Page* pg = pg_res.value();
                        const LinePointer* lp = pg->line_pointer(ih.slot_idx);
                        if (lp && lp->is_valid() && lp->offset + 8 <= kPageSize) {
                            u64 slot_xmin = 0;
                            std::memcpy(&slot_xmin, pg->data() + lp->offset, 8);
                            db->pool().unpin_page(ih.page_id);
                            if (slot_xmin == ref.hdr.txn_id) {
                                heap->rollback_insert(ih.page_id, ih.slot_idx, ref.hdr.lsn);
                                needs_index_rebuild = true;
                            }
                        } else {
                            db->pool().unpin_page(ih.page_id);
                        }
                    }
                }
            } else if (ref.hdr.type == WalType::kDelete) {
                struct __attribute__((packed)) DeleteHdr {
                    u32 table_id;
                    u64 page_id;
                    u16 slot_idx;
                };
                if (data.size() < sizeof(DeleteHdr)) continue;
                DeleteHdr dh;
                std::memcpy(&dh, data.data(), sizeof(dh));
                HeapFile* heap = db->get_heap_file(dh.table_id);
                if (heap) {
                    heap->mark_deleted(dh.page_id, dh.slot_idx, kInvalidTxnId, ref.hdr.lsn);
                    needs_index_rebuild = true;
                }
            } else if (ref.hdr.type == WalType::kUpdate) {
                struct __attribute__((packed)) UpdateHdr {
                    u32 table_id;
                    u64 old_page_id;
                    u16 old_slot_idx;
                    u64 new_page_id;
                    u16 new_slot_idx;
                    u16 data_size;
                };
                if (data.size() < sizeof(UpdateHdr)) continue;
                UpdateHdr uh;
                std::memcpy(&uh, data.data(), sizeof(uh));
                HeapFile* heap = db->get_heap_file(uh.table_id);
                if (heap) {
                    heap->mark_deleted(uh.old_page_id, uh.old_slot_idx, kInvalidTxnId, ref.hdr.lsn);
                    heap->rollback_insert(uh.new_page_id, uh.new_slot_idx, ref.hdr.lsn);
                    needs_index_rebuild = true;
                }
            }
        }
    }
    close(fd);

    // Advance — never regress — the LSN counters. If load_control_file()
    // already restored a higher checkpoint_lsn watermark before recover()
    // ran, keep that value: LSNs must stay globally monotonic across
    // restarts even when the WAL file itself was truncated by clean
    // shutdown. Resetting to max_lsn+1 here would re-introduce the D2
    // checkpoint deadlock (page_lsn > durable_lsn → flush_until under
    // held WAL latch).
    if (max_lsn + 1 > next_lsn_)      next_lsn_ = max_lsn + 1;
    if (max_lsn > durable_lsn_)       durable_lsn_ = max_lsn;
    if (max_lsn > last_written_lsn_)  last_written_lsn_ = max_lsn;
    if (db && max_txn_id > 0) {
        db->txn_manager().ensure_next_txn_id_at_least(max_txn_id + 1);
    }
    return needs_index_rebuild;
}

} // namespace minidb
