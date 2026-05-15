/**
 * @file lock_manager.h
 * @brief 锁管理器 — 表级锁, 兼容矩阵, 等待Queue, 死锁检测
 *
 * 对齐 PostgreSQL LockManager 概念:
 *   - 锁模式: AccessShare, RowExclusive, Exclusive, AccessExclusive
 *   - 锁兼容矩阵控制并发
 *   - 等待Queue + 超时
 *   - wait-for graph 死锁检测
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
// Lock模式 (对齐 PostgreSQL, 简化为 4 种)
// ============================================================

enum class LockMode : u8 {
    kAccessShare      = 0,  // SELECT (读)
    kRowExclusive     = 1,  // INSERT/UPDATE/DELETE (写)
    kExclusive        = 2,  // CREATE INDEX (独占但允许读)
    kAccessExclusive  = 3,  // DROP TABLE/ALTER TABLE (完全独占)
};

static constexpr u32 kNumLockModes = 4;

// ============================================================
// Lock请求
// ============================================================

struct LockRequest {
    u64       txn_id;       // 请求事务 ID
    LockMode  mode;         // 请求的锁模式
    bool      granted;      // 是否已授予
    bool      waiting;      // 是否正在等待
};

// ============================================================
// Lock对象 (每个表一个)
// ============================================================

struct LockObject {
    u32                    table_id;
    Vector<LockRequest>    requests;     // 等待Queue
    LockMode               granted_mask; // 当前已授予的最高锁模式
};

// ============================================================
// LockManager
// ============================================================

class LockManager {
public:
    LockManager();
    ~LockManager() = default;

    // 请求锁 (阻塞直到Get或超时)
    Status lock_table(u64 txn_id, u32 table_id, LockMode mode);
    Status lock_record(u64 txn_id, u32 table_id, const RecordId& rid, LockMode mode);
    Status lock_key(u64 txn_id, u32 table_id, const String& key, LockMode mode);

    // Free锁
    void unlock_table(u64 txn_id, u32 table_id);
    void unlock_record(u64 txn_id, u32 table_id, const RecordId& rid);
    void unlock_key(u64 txn_id, u32 table_id, const String& key);

    // Free all locks held by the transaction (事务结束时调用)
    void unlock_all(u64 txn_id);

    // Deadlock检测
    bool detect_deadlock(u64 txn_id);

private:
    bool is_compatible(LockMode held, LockMode requested) const;
    bool can_grant(const LockObject& obj, u64 txn_id, LockMode mode) const;
    LockMode max_mode(LockMode a, LockMode b) const;
    void grant_pending(LockObject* obj);

    Mutex latch_;
    CondVar cond_;
    HashMap<u32, LockObject> lock_table_;   // table_id → LockObject
    HashMap<u64, Vector<u32>> txn_locks_;   // txn_id → 持有的 table_id 列表
    HashMap<u64, LockObject> record_locks_; // record/key → LockObject
    HashMap<u64, Vector<u64>> txn_record_locks_;
};

} // namespace minidb
