/**
 * @file lock_manager.h
 * @brief Lock manager — table-level locks, compatibility matrix, wait queue, deadlock detection.
 *
 * Mirrors PostgreSQL LockManager concepts:
 *   - lock modes: AccessShare, RowExclusive, Exclusive, AccessExclusive
 *   - lock compatibility matrix governs concurrency
 *   - wait queue + timeout
 *   - wait-for graph for deadlock detection
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "common/status.h"
#include "container/string.h"
#include "container/vector.h"
#include "container/hash_map.h"
#include "index/btree.h"

namespace minidb {

// ============================================================
// Lock modes (PostgreSQL-aligned, simplified to 4 modes).
// ============================================================

enum class LockMode : u8 {
    kAccessShare      = 0,  // SELECT (read)
    kRowExclusive     = 1,  // INSERT/UPDATE/DELETE (write)
    kExclusive        = 2,  // CREATE INDEX (exclusive, still allows reads)
    kAccessExclusive  = 3,  // DROP TABLE / ALTER TABLE (fully exclusive)
};

static constexpr u32 kNumLockModes = 4;

// ============================================================
// Lock request.
// ============================================================

struct LockRequest {
    u64       txn_id;       // requesting transaction id
    LockMode  mode;         // requested lock mode
    bool      granted;      // true once granted
    bool      waiting;      // true while waiting on the queue
};

// ============================================================
// Lock object (one per table).
// ============================================================

struct LockObject {
    u32                    table_id;
    Vector<LockRequest>    requests;     // wait queue
    LockMode               granted_mask; // strongest granted mode currently held
};

// ============================================================
// LockManager
// ============================================================

class LockManager {
public:
    LockManager();
    ~LockManager() = default;

    // Acquire a lock (blocks until granted or timed out).
    Status lock_table(u64 txn_id, u32 table_id, LockMode mode);
    Status lock_record(u64 txn_id, u32 table_id, const RecordId& rid, LockMode mode);
    Status lock_key(u64 txn_id, u32 table_id, const String& key, LockMode mode);

    // Release a lock.
    void unlock_table(u64 txn_id, u32 table_id);
    void unlock_record(u64 txn_id, u32 table_id, const RecordId& rid);
    void unlock_key(u64 txn_id, u32 table_id, const String& key);

    // Release every lock held by the transaction (called on commit/rollback).
    void unlock_all(u64 txn_id);

    // Deadlock detection.
    bool detect_deadlock(u64 txn_id);

private:
    bool is_compatible(LockMode held, LockMode requested) const;
    bool can_grant(const LockObject& obj, u64 txn_id, LockMode mode) const;
    LockMode max_mode(LockMode a, LockMode b) const;
    void grant_pending(LockObject* obj);

    Mutex latch_;
    CondVar cond_;
    HashMap<u32, LockObject> lock_table_;   // table_id → LockObject
    HashMap<u64, Vector<u32>> txn_locks_;   // txn_id -> list of held table_ids
    HashMap<u64, LockObject> record_locks_; // record/key → LockObject
    HashMap<u64, Vector<u64>> txn_record_locks_;
};

} // namespace minidb
