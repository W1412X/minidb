/**
 * @file transaction.h
 * @brief MVCC Transaction Manager — snapshot_id, commit_id, visibility check
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "container/vector.h"
#include "container/unique_ptr.h"
#include "index/btree.h"
#include "record/value.h"
#include "transaction/txn_status_log.h"

namespace minidb {

class Database;

enum class UndoType : u8 {
    kInsert = 0,
    kDelete = 1,
    kHotInsert = 2,
    kHotDelete = 3
};

struct UndoRecord {
    UndoType type;
    u32 table_id;
    RecordId rid;
};

static constexpr u64 kInvalidTxnId = 0;

enum class TxnState : u8 { kActive = 0, kCommitted = 1, kAborted = 2 };

struct TxnSlot {
    u64     txn_id;          // transaction unique id
    u64     snapshot_id;     // snapshot: sees all versions committed before snapshot_id
    u64     commit_id;       // assigned at commit (0 = uncommitted)
    TxnState state;
    PageId  home_page;       // Page lock holding info for the transaction's process (reserved)
};

class Transaction {
public:
    Transaction(u64 txn_id, u64 snapshot_id, Vector<u64>&& active_snapshot);
    ~Transaction() = default;

    u64 id() const { return txn_id_; }
    u64 snapshot_id() const { return snapshot_id_; }
    TxnState state() const { return state_; }
    u64 commit_id() const { return commit_id_; }
    const Vector<UndoRecord>& undo_records() const { return undo_records_; }
    const Vector<u64>& active_snapshot() const { return active_snapshot_; }

    void set_state(TxnState s) { state_ = s; }
    void set_commit_id(u64 id) { commit_id_ = id; }
    void set_resource_acquired(bool v) { resource_acquired_ = v; }
    bool resource_acquired() const { return resource_acquired_; }
    void record_insert(u32 table_id, const RecordId& rid);
    void record_delete(u32 table_id, const RecordId& rid);
    void record_hot_insert(u32 table_id, const RecordId& rid);
    void record_hot_delete(u32 table_id, const RecordId& rid);

    // Mark a point in the undo log that the statement-level rollback can
    // restore. Returns the current undo log length.
    u32 undo_mark() const { return undo_records_.size(); }
    // Drop undo records past `mark`. Used by rollback_to_savepoint after
    // it has applied them in reverse.
    void truncate_undo(u32 mark);

private:
    u64 txn_id_;
    u64 snapshot_id_;
    u64 commit_id_;
    TxnState state_;
    Vector<u64> active_snapshot_;
    Vector<UndoRecord> undo_records_;
    bool resource_acquired_;
};

class TransactionManager {
public:
    explicit TransactionManager(Database* db);
    ~TransactionManager() = default;

    // Transaction lifecycle
    Transaction* begin();
    bool commit(Transaction* txn);
    bool rollback(Transaction* txn);
    // Undo every change recorded after `mark` in the txn's undo log, in
    // reverse order, then truncate the log back to that point. The
    // transaction stays ACTIVE — this is the engine behind statement-
    // level atomicity inside an explicit BEGIN..COMMIT.
    bool rollback_to_savepoint(Transaction* txn, u32 mark);
    void record_insert(u32 table_id, const RecordId& rid);
    void record_delete(u32 table_id, const RecordId& rid);
    void record_hot_insert(u32 table_id, const RecordId& rid);
    void record_hot_delete(u32 table_id, const RecordId& rid);

    // Snapshot query
    Transaction* current() const;
    u64 next_snapshot_id();

    // Visibility predicate (called by SeqScan etc.)
    // A version V is visible to transaction T iff
    //   1. V.xmin is committed AND V.xmin < T.snapshot, AND
    //   2. V.xmax == 0, or V.xmax is not committed, or V.xmax >= T.snapshot
    bool is_visible(u64 xmin, u64 xmax, const Transaction& txn) const;

    bool is_txn_committed(u64 txn_id) const;
    bool get_txn_state(u64 txn_id, TxnState* out) const;
    u64  get_commit_id(u64 txn_id) const;
    u64  get_oldest_active_txn_id() const;
    void ensure_next_txn_id_at_least(u64 next_id);
    u64  next_txn_id() const;

    TxnSlot* txn_slots() { return txn_slots_.data(); }
    u32 txn_slot_count() const { return txn_slots_.size(); }

    // Access to the persistent xid → final-state log (A1). Lives behind a
    // UniquePtr so the Database constructor can hand in the db_dir; nullptr
    // when the manager runs without a backing directory (tests).
    TxnStatusLog* status_log() const { return status_log_.get(); }
    void set_status_log(UniquePtr<TxnStatusLog> log);

private:
    TxnSlot* find_slot(u64 txn_id);
    TxnSlot* find_active_slot(u64 txn_id);
    TxnSlot* alloc_slot(u64 txn_id, u64 snapshot_id);

    Vector<TxnSlot> txn_slots_;

    mutable Mutex latch_;
    Database* db_;
    u64 next_txn_id_;
    UniquePtr<TxnStatusLog> status_log_;
};

} // namespace minidb
