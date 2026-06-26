/**
 * @file wal.h
 * @brief WAL (Write-Ahead Log) — Crash recovery
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "common/atomic.h"
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
    kDdl         = 40,    // DDL audit marker, see DdlOp below
    // Compensating log records emitted during statement-level savepoint
    // rollback. They tell recovery to undo a specific INSERT or DELETE
    // record that was applied earlier in the same transaction. Without
    // these, recovery would re-do the original INSERT/DELETE even though
    // the statement that produced it was rolled back in memory.
    kSavepointUndoInsert = 50,   // payload: u32 table_id | u64 page_id | u16 slot_idx
    kSavepointUndoDelete = 51,   // same payload — undoes a prior xmax stamp
};

// DDL operations that produce a kDdl record. The marker is purely an
// audit trail today — recovery does not act on it yet — but it lets a
// future repair pass detect orphaned files / half-finished schema
// changes without scanning the entire filesystem. ACID_TODO D7.
enum class DdlOp : u8 {
    kCreateTable        = 1,
    kDropTable          = 2,
    kCreateIndex        = 3,
    kDropIndex          = 4,
    kAlterAddColumn     = 5,
    kAlterDropColumn    = 6,
    kAlterRenameColumn  = 7,
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
    u64 log_index_insert(u64 txn_id, u32 index_id, const IndexKey& key, const RecordId& rid);
    u64 log_index_delete(u64 txn_id, u32 index_id, const IndexKey& key, const RecordId& rid);
    // Statement-level savepoint compensating records. Written when an
    // explicit-transaction statement bails out partway and its in-memory
    // writes are rolled back. Recovery re-applies the original WAL
    // record AND then this compensating record, leaving the heap in the
    // same shape that the live database has at commit time.
    u64 log_savepoint_undo_insert(u64 txn_id, u32 table_id, PageId page_id, SlotIdx slot_idx);
    u64 log_savepoint_undo_delete(u64 txn_id, u32 table_id, PageId page_id, SlotIdx slot_idx);
    // Emit a kDdl record. `object_name` is the table/index/column
    // affected by the operation; `aux` carries a secondary identifier
    // (the column index for ALTER, the index id for CREATE/DROP INDEX,
    // 0 otherwise). Records are written ON SUCCESS so a kDdl in the log
    // means the operation reached its in-memory completion point.
    // During recovery, uncommitted DDL records trigger the reverse
    // operation to restore catalog consistency.
    u64 log_ddl(u64 txn_id, DdlOp op, u32 table_id, u32 aux, const String& object_name);

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
    u64 durable_lsn() const { return durable_lsn_.load(); }
    u64 next_lsn() const { return next_lsn_.load(); }
    u64 bytes_since_checkpoint() const { return bytes_since_checkpoint_.load(); }
    u64 group_commit_batches() const { return group_commit_batches_.load(); }
    u64 buffer_flushes() const { return buffer_flushes_.load(); }
    u64 buffered_bytes() const { return buffered_bytes_.load(); }

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
    // These counters are read locklessly from other threads (the background
    // maintenance loop checks bytes_since_checkpoint_; the buffer pool reads
    // durable_lsn_ on the eviction/flush hot path; stats_summary reads all of
    // them). All mutations happen under latch_, so the atomics only guarantee
    // torn-free cross-thread reads — no extra serialization is implied.
    Atomic<u64> next_lsn_;
    Atomic<u64> durable_lsn_;
    u64 last_written_lsn_;
    u64 segment_size_bytes_;
    bool fsync_enabled_;
    bool group_commit_enabled_;
    u64 group_commit_delay_ms_;
    u32 pending_commit_waiters_;
    Atomic<u64> group_commit_batches_;
    // Monotonic counter incremented at the end of every group-commit batch.
    // Followers compare against the value they captured on entry so they
    // notice a closed batch even if their LSN never became durable.
    u64 commit_batch_id_;
    Atomic<u64> buffer_flushes_;
    Atomic<u64> buffered_bytes_;
    Atomic<u64> bytes_since_checkpoint_;
    Mutex latch_;
    CondVar commit_cond_;

    static constexpr u32 kWalBufferSize = 8192;
    byte write_buf_[kWalBufferSize];
    u32 write_buf_pos_ = 0;
};

} // namespace minidb
