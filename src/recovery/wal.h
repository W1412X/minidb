/**
 * @file wal.h
 * @brief WAL (Write-Ahead Log) — Crash recovery
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "container/string.h"
#include "index/btree.h"
#include "record/value.h"

namespace minidb {

class Database;

enum class WalType : u16 {
    kTxnBegin    = 1,
    kTxnCommit   = 2,
    kTxnAbort    = 3,
    kInsert      = 10,
    kDelete      = 11,
    kUpdate      = 12,
    kIndexInsert = 13,
    kIndexDelete = 14,
    kPageAlloc   = 20,
    kCheckpoint  = 30,
};

// Magic prefix on every WAL record. A torn or wild write that overwrites
// part of the log will almost certainly produce a header whose first four
// bytes do not match this constant, so replay can stop cleanly instead of
// re-interpreting garbage as a record.
static constexpr u32 kWalRecordMagic = 0xD8BA110Cu;

struct WalRecord {
    u32     magic;     // == kWalRecordMagic
    u32     crc;       // CRC32 over the header (with crc=0) and the payload
    u64     lsn;       // Log Sequence Number
    u64     txn_id;
    WalType type;
    u32     data_len;
    // Followed by data_len bytes of data
};

class WalManager {
public:
    explicit WalManager(const String& wal_dir);
    WalManager(const String& wal_dir, u64 segment_size_bytes, bool fsync_enabled);
    WalManager(const String& wal_dir, u64 segment_size_bytes, bool fsync_enabled,
               bool group_commit_enabled, u64 group_commit_delay_ms);
    ~WalManager();

    u64 log_begin(u64 txn_id);
    u64 log_commit(u64 txn_id);
    u64 log_abort(u64 txn_id);
    u64 log_insert(u64 txn_id, u32 table_id, PageId page_id, SlotIdx slot_idx,
                   const byte* data, u16 size);
    u64 log_delete(u64 txn_id, u32 table_id, PageId page_id, SlotIdx slot_idx);
    u64 log_update(u64 txn_id, u32 table_id,
                   PageId old_page_id, SlotIdx old_slot_idx,
                   PageId new_page_id, SlotIdx new_slot_idx,
                   const byte* new_data, u16 size);
    u64 log_index_insert(u64 txn_id, u32 index_id, const Value& key, const RecordId& rid);
    u64 log_index_delete(u64 txn_id, u32 index_id, const Value& key, const RecordId& rid);

    // Hook invoked inside `checkpoint()` while the WAL latch is held, after
    // the kCheckpoint record has been fsynced and before the log is
    // truncated. Used to flush dirty pages so that no committed page write
    // depends on a WAL record we are about to truncate.
    using CheckpointPageFlush = void (*)(void* ctx);
    u64 checkpoint(CheckpointPageFlush flush_pages_cb, void* ctx);
    u64 checkpoint() { return checkpoint(nullptr, nullptr); }

    void flush();
    bool flush_until(u64 lsn);
    bool recover(Database* db);
    // Restore the LSN watermark across restarts. The on-disk WAL file is
    // truncated by clean shutdown so next_lsn_ would otherwise reset to 1,
    // but pages on disk still carry the higher LSNs from prior sessions.
    // The Database constructor reads the persisted checkpoint LSN from the
    // control file and calls this to keep LSNs globally monotonic. Without
    // it, a checkpoint after restart could write a kCheckpoint record with
    // an LSN smaller than existing page LSNs, which would then trigger a
    // flush_until() recursion under the D2 checkpoint barrier and deadlock.
    void ensure_next_lsn_at_least(u64 lsn);
    u64 durable_lsn() const { return durable_lsn_; }
    u64 next_lsn() const { return next_lsn_; }
    u64 bytes_since_checkpoint() const { return bytes_since_checkpoint_; }
    u64 group_commit_batches() const { return group_commit_batches_; }
    u64 buffer_flushes() const { return buffer_flushes_; }
    u64 buffered_bytes() const { return buffered_bytes_; }

private:
    u64 write_record(WalType type, u64 txn_id, const byte* data, u32 data_len);
    bool append_to_buffer(const byte* data, u32 len);
    bool write_direct(const byte* data, u32 len);
    bool flush_buffer();
    // Returns true iff `lsn` is durable on disk when the call returns.
    bool flush_commit(u64 lsn);
    void truncate();

    String wal_dir_;
    int fd_;
    u64 next_lsn_;
    u64 durable_lsn_;
    u64 last_written_lsn_;
    u64 segment_size_bytes_;
    bool fsync_enabled_;
    bool group_commit_enabled_;
    u64 group_commit_delay_ms_;
    u32 pending_commit_waiters_;
    u64 group_commit_batches_;
    // Monotonic counter incremented at the end of every group-commit batch.
    // Followers compare against the value they captured on entry so they
    // notice a closed batch even if their LSN never became durable.
    u64 commit_batch_id_;
    u64 buffer_flushes_;
    u64 buffered_bytes_;
    u64 bytes_since_checkpoint_;
    Mutex latch_;
    CondVar commit_cond_;

    static constexpr u32 kWalBufferSize = 8192;
    byte write_buf_[kWalBufferSize];
    u32 write_buf_pos_ = 0;
};

} // namespace minidb
