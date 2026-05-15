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

struct WalRecord {
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
    u64 checkpoint();

    void flush();
    bool recover(Database* db);
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
    void flush_buffer();
    void flush_commit(u64 lsn);
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
