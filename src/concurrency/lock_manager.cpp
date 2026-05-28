/**
 * @file lock_manager.cpp
 * @brief Lock manager implementation
 *
 * Lock compatibility matrix (aligned with PostgreSQL):
 *
 *                Request mode
 * Held mode       AS    RX    X     AX
 * AS            ✅    ✅    ✅    ❌
 * RX            ✅    ✅    ❌    ❌
 * X             ✅    ❌    ❌    ❌
 * AX            ❌    ❌    ❌    ❌
 */
#include "concurrency/lock_manager.h"
#include "common/trace.h"
#include <cstring>
#include <chrono>

namespace minidb {

// Lock compatibility matrix: compatible[held][requested]
static const bool kLockCompatibility[kNumLockModes][kNumLockModes] = {
    // AS    RX    X     AX
    { true,  true,  true,  false },  // held = AccessShare
    { true,  true,  false, false },  // held = RowExclusive
    { true,  false, false, false },  // held = Exclusive
    { false, false, false, false },  // held = AccessExclusive
};

LockManager::LockManager() {}

static u64 record_lock_key(u32 table_id, const RecordId& rid) {
    u64 h = static_cast<u64>(table_id) * 11400714819323198485ull;
    h ^= rid.page_id + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= static_cast<u64>(rid.slot_idx) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

static u64 logical_lock_key(u32 table_id, const String& key) {
    u64 h = static_cast<u64>(table_id) * 1099511628211ull;
    h ^= Hash<String>()(key);
    h ^= 0xD15EA5E5D15EA5E5ull;
    return h;
}

bool LockManager::is_compatible(LockMode held, LockMode requested) const {
    return kLockCompatibility[static_cast<u8>(held)][static_cast<u8>(requested)];
}

LockMode LockManager::max_mode(LockMode a, LockMode b) const {
    return static_cast<LockMode>((static_cast<u8>(a) > static_cast<u8>(b))
                                  ? static_cast<u8>(a) : static_cast<u8>(b));
}

bool LockManager::can_grant(const LockObject& obj, u64 txn_id, LockMode mode) const {
    for (u32 i = 0; i < obj.requests.size(); i++) {
        const LockRequest& req = obj.requests[i];
        if (!req.granted || req.txn_id == txn_id) continue;
        if (!is_compatible(req.mode, mode) || !is_compatible(mode, req.mode)) {
            return false;
        }
    }
    return true;
}

void LockManager::grant_pending(LockObject* obj) {
    for (u32 i = 0; i < obj->requests.size(); i++) {
        if (obj->requests[i].granted) continue;
        if (can_grant(*obj, obj->requests[i].txn_id, obj->requests[i].mode)) {
            obj->requests[i].granted = true;
            obj->requests[i].waiting = false;
        }
    }

    LockMode current_held = LockMode::kAccessShare;
    for (u32 i = 0; i < obj->requests.size(); i++) {
        if (obj->requests[i].granted) {
            current_held = max_mode(current_held, obj->requests[i].mode);
        }
    }
    obj->granted_mask = current_held;
}

Status LockManager::lock_table(u64 txn_id, u32 table_id, LockMode mode) {
    auto trace_start = std::chrono::steady_clock::now();
    bool trace_waited = false;
    latch_.lock();

    LockObject* obj = lock_table_.find(table_id);
    if (!obj) {
        // Create new lock object
        LockObject new_obj;
        new_obj.table_id = table_id;
        new_obj.granted_mask = LockMode::kAccessShare;
        lock_table_[table_id] = new_obj;
        obj = lock_table_.find(table_id);
    }

    // Check if already holding same or stronger lock
    for (u32 i = 0; i < obj->requests.size(); i++) {
        if (obj->requests[i].txn_id == txn_id && obj->requests[i].granted) {
            if (static_cast<u8>(obj->requests[i].mode) >= static_cast<u8>(mode)) {
                latch_.unlock();
                if (TraceContext* trace = current_trace()) {
                    trace->record_lock("table", txn_id, table_id, false, 0, true);
                }
                return Status::ok_status();  // Already holding sufficient lock
            }
            // Lock upgrade: already hold a weaker mode, attempt in-place upgrade (avoids deadlock).
            // First check the new mode is compatible with every other granted lock.
            bool can_upgrade = true;
            for (u32 j = 0; j < obj->requests.size(); j++) {
                if (j == i) continue;  // skip self
                if (obj->requests[j].granted &&
                    !is_compatible(obj->requests[j].mode, mode)) {
                    can_upgrade = false;
                    break;
                }
            }
            if (can_upgrade) {
                obj->requests[i].mode = mode;
                obj->requests[i].granted = true;
                obj->requests[i].waiting = false;
                // Recompute granted_mask.
                LockMode max_held = LockMode::kAccessShare;
                for (u32 k = 0; k < obj->requests.size(); k++) {
                    if (obj->requests[k].granted) {
                        max_held = max_mode(max_held, obj->requests[k].mode);
                    }
                }
                obj->granted_mask = max_held;
                latch_.unlock();
                if (TraceContext* trace = current_trace()) {
                    trace->record_lock("table", txn_id, table_id, false, 0, true);
                }
                return Status::ok_status();
            }
            // Cannot upgrade: another txn holds an incompatible lock; surface conflict, not deadlock.
            latch_.unlock();
            if (TraceContext* trace = current_trace()) {
                trace->record_lock("table", txn_id, table_id, false, 0, false);
            }
            return Status(ErrorCode::kLockConflict, "lock upgrade blocked by other holders");
        }
    }

    // Append the request.
    LockRequest req;
    req.txn_id = txn_id;
    req.mode = mode;
    req.granted = false;
    req.waiting = true;
    obj->requests.push_back(req);

    static constexpr u32 kLockWaitTimeoutMs = 2000;
    while (true) {
        grant_pending(obj);
        for (u32 i = 0; i < obj->requests.size(); i++) {
            if (obj->requests[i].txn_id == txn_id &&
                obj->requests[i].mode == mode && obj->requests[i].granted) {
                Vector<u32>* txn_lock_list = txn_locks_.find(txn_id);
                if (!txn_lock_list) {
                    txn_locks_[txn_id] = Vector<u32>();
                    txn_lock_list = txn_locks_.find(txn_id);
                }
                bool found = false;
                for (u32 j = 0; j < txn_lock_list->size(); j++) {
                    if ((*txn_lock_list)[j] == table_id) { found = true; break; }
                }
                if (!found) txn_lock_list->push_back(table_id);
                latch_.unlock();
                if (TraceContext* trace = current_trace()) {
                    u64 wait_us = static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - trace_start).count());
                    trace->record_lock("table", txn_id, table_id, trace_waited, trace_waited ? wait_us : 0, true);
                }
                return Status::ok_status();
            }
        }

        if (detect_deadlock(txn_id)) {
            break;
        }

        trace_waited = true;
        if (!cond_.timed_wait(latch_, kLockWaitTimeoutMs)) {
            break;
        }
        obj = lock_table_.find(table_id);
        if (!obj) break;
    }

    for (u32 i = 0; obj && i < obj->requests.size(); i++) {
        if (obj->requests[i].txn_id == txn_id &&
            obj->requests[i].mode == mode && !obj->requests[i].granted) {
            for (u32 j = i; j < obj->requests.size() - 1; j++) {
                obj->requests[j] = obj->requests[j + 1];
            }
            obj->requests.resize(obj->requests.size() - 1);
            break;
        }
    }
    latch_.unlock();
    if (TraceContext* trace = current_trace()) {
        u64 wait_us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - trace_start).count());
        trace->record_lock("table", txn_id, table_id, trace_waited, trace_waited ? wait_us : 0, false);
    }
    return Status(ErrorCode::kLockConflict, "lock wait timeout or deadlock");
}

Status LockManager::lock_record(u64 txn_id, u32 table_id, const RecordId& rid, LockMode mode) {
    auto trace_start = std::chrono::steady_clock::now();
    bool trace_waited = false;
    u64 key = record_lock_key(table_id, rid);
    latch_.lock();
    LockObject* obj = record_locks_.find(key);
    if (!obj) {
        LockObject new_obj;
        new_obj.table_id = table_id;
        new_obj.granted_mask = LockMode::kAccessShare;
        record_locks_[key] = new_obj;
        obj = record_locks_.find(key);
    }

    for (u32 i = 0; i < obj->requests.size(); i++) {
        if (obj->requests[i].txn_id == txn_id && obj->requests[i].granted) {
            if (static_cast<u8>(obj->requests[i].mode) >= static_cast<u8>(mode)) {
                latch_.unlock();
                if (TraceContext* trace = current_trace()) {
                    trace->record_lock("record", txn_id, table_id, false, 0, true);
                }
                return Status::ok_status();
            }
            latch_.unlock();
            if (TraceContext* trace = current_trace()) {
                trace->record_lock("record", txn_id, table_id, false, 0, false);
            }
            return Status(ErrorCode::kLockConflict, "record lock upgrade blocked");
        }
    }

    LockRequest req;
    req.txn_id = txn_id;
    req.mode = mode;
    req.granted = false;
    req.waiting = true;
    obj->requests.push_back(req);

    static constexpr u32 kLockWaitTimeoutMs = 2000;
    while (true) {
        grant_pending(obj);
        for (u32 i = 0; i < obj->requests.size(); i++) {
            if (obj->requests[i].txn_id == txn_id &&
                obj->requests[i].mode == mode && obj->requests[i].granted) {
                Vector<u64>* held = txn_record_locks_.find(txn_id);
                if (!held) {
                    txn_record_locks_[txn_id] = Vector<u64>();
                    held = txn_record_locks_.find(txn_id);
                }
                bool found = false;
                for (u32 j = 0; j < held->size(); j++) {
                    if ((*held)[j] == key) { found = true; break; }
                }
                if (!found) held->push_back(key);
                latch_.unlock();
                if (TraceContext* trace = current_trace()) {
                    u64 wait_us = static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - trace_start).count());
                    trace->record_lock("record", txn_id, table_id, trace_waited,
                                       trace_waited ? wait_us : 0, true);
                }
                return Status::ok_status();
            }
        }
        trace_waited = true;
        if (!cond_.timed_wait(latch_, kLockWaitTimeoutMs)) {
            // W14: Timeout — check for deadlock and select victim
            obj = record_locks_.find(key);
            if (!obj) break;

            // Find the youngest txn in the wait chain as victim
            u64 victim_txn = txn_id;
            for (u32 i = 0; i < obj->requests.size(); i++) {
                if (obj->requests[i].granted && obj->requests[i].txn_id > victim_txn) {
                    victim_txn = obj->requests[i].txn_id;
                }
            }

            // If we are the victim, abort ourselves
            if (victim_txn == txn_id) {
                // Remove our pending request
                for (u32 i = 0; obj && i < obj->requests.size(); i++) {
                    if (obj->requests[i].txn_id == txn_id &&
                        obj->requests[i].mode == mode && !obj->requests[i].granted) {
                        for (u32 j = i; j < obj->requests.size() - 1; j++) {
                            obj->requests[j] = obj->requests[j + 1];
                        }
                        obj->requests.resize(obj->requests.size() - 1);
                        break;
                    }
                }
                latch_.unlock();
                if (TraceContext* trace = current_trace()) {
                    u64 wait_us = static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - trace_start).count());
                    trace->record_lock("record", txn_id, table_id, true, wait_us, false);
                }
                return Status(ErrorCode::kLockConflict, "deadlock detected, this transaction chosen as victim");
            }

            // Otherwise, force-release the victim's locks to break the deadlock
            // Remove victim's granted request from this lock object
            for (u32 i = 0; obj && i < obj->requests.size(); i++) {
                if (obj->requests[i].txn_id == victim_txn && obj->requests[i].granted) {
                    for (u32 j = i; j < obj->requests.size() - 1; j++) {
                        obj->requests[j] = obj->requests[j + 1];
                    }
                    obj->requests.resize(obj->requests.size() - 1);
                    break;
                }
            }
            // Update granted_mask
            LockMode max_held = LockMode::kAccessShare;
            for (u32 i = 0; obj && i < obj->requests.size(); i++) {
                if (obj->requests[i].granted) {
                    max_held = max_mode(max_held, obj->requests[i].mode);
                }
            }
            obj->granted_mask = max_held;
            cond_.broadcast();
            // Continue waiting for our turn
            obj = record_locks_.find(key);
            if (!obj) break;
            continue;
        }
        obj = record_locks_.find(key);
        if (!obj) break;
    }

    for (u32 i = 0; obj && i < obj->requests.size(); i++) {
        if (obj->requests[i].txn_id == txn_id &&
            obj->requests[i].mode == mode && !obj->requests[i].granted) {
            for (u32 j = i; j < obj->requests.size() - 1; j++) {
                obj->requests[j] = obj->requests[j + 1];
            }
            obj->requests.resize(obj->requests.size() - 1);
            break;
        }
    }
    latch_.unlock();
    if (TraceContext* trace = current_trace()) {
        u64 wait_us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - trace_start).count());
        trace->record_lock("record", txn_id, table_id, trace_waited,
                           trace_waited ? wait_us : 0, false);
    }
    return Status(ErrorCode::kLockConflict, "record lock wait timeout");
}

Status LockManager::lock_key(u64 txn_id, u32 table_id, const String& key, LockMode mode) {
    RecordId pseudo(logical_lock_key(table_id, key), 0);
    return lock_record(txn_id, table_id, pseudo, mode);
}

void LockManager::unlock_table(u64 txn_id, u32 table_id) {
    LockGuard guard(latch_);

    LockObject* obj = lock_table_.find(table_id);
    if (!obj) return;

    // Remove every request from this transaction.
    Vector<LockRequest> remaining;
    for (u32 i = 0; i < obj->requests.size(); i++) {
        if (obj->requests[i].txn_id != txn_id) {
            remaining.push_back(obj->requests[i]);
        }
    }
    obj->requests = remaining;

    // Recompute granted_mask and grant waiting requests.
    obj->granted_mask = LockMode::kAccessShare;
    grant_pending(obj);
    cond_.broadcast();

    // Remove from the per-transaction lock list.
    Vector<u32>* txn_lock_list = txn_locks_.find(txn_id);
    if (txn_lock_list) {
        Vector<u32> remaining_ids;
        for (u32 i = 0; i < txn_lock_list->size(); i++) {
            if ((*txn_lock_list)[i] != table_id) {
                remaining_ids.push_back((*txn_lock_list)[i]);
            }
        }
        *txn_lock_list = remaining_ids;
    }
}

void LockManager::unlock_record(u64 txn_id, u32 table_id, const RecordId& rid) {
    (void)table_id;
    u64 key = record_lock_key(table_id, rid);
    LockGuard guard(latch_);
    LockObject* obj = record_locks_.find(key);
    if (!obj) return;
    Vector<LockRequest> remaining;
    for (u32 i = 0; i < obj->requests.size(); i++) {
        if (obj->requests[i].txn_id != txn_id) remaining.push_back(obj->requests[i]);
    }
    obj->requests = remaining;
    grant_pending(obj);
    cond_.broadcast();
}

void LockManager::unlock_key(u64 txn_id, u32 table_id, const String& key) {
    RecordId pseudo(logical_lock_key(table_id, key), 0);
    unlock_record(txn_id, table_id, pseudo);
}

void LockManager::unlock_all(u64 txn_id) {
    Vector<u32> table_ids;
    {
        LockGuard guard(latch_);
        Vector<u32>* txn_lock_list = txn_locks_.find(txn_id);
        if (!txn_lock_list) return;
        for (u32 i = 0; i < txn_lock_list->size(); i++) {
            table_ids.push_back((*txn_lock_list)[i]);
        }
    }

    for (u32 i = 0; i < table_ids.size(); i++) {
        unlock_table(txn_id, table_ids[i]);
    }

    LockGuard guard(latch_);
    Vector<u64>* record_ids = txn_record_locks_.find(txn_id);
    if (record_ids) {
        for (u32 r = 0; r < record_ids->size(); r++) {
            LockObject* obj = record_locks_.find((*record_ids)[r]);
            if (!obj) continue;
            Vector<LockRequest> remaining;
            for (u32 i = 0; i < obj->requests.size(); i++) {
                if (obj->requests[i].txn_id != txn_id) remaining.push_back(obj->requests[i]);
            }
            obj->requests = remaining;
            grant_pending(obj);
        }
        txn_record_locks_.erase(txn_id);
        cond_.broadcast();
    }
    txn_locks_.erase(txn_id);
}

bool LockManager::detect_deadlock(u64 txn_id) {
    // Deadlock detection: check if txn_id is in a cycle in the wait-for graph.
    // Instead of building a full graph (which loses multi-edge info), we follow
    // edges on-the-fly by scanning all lock objects for each transaction.

    auto find_holder_for = [this](u64 waiter) -> Vector<u64> {
        Vector<u64> holders;
        for (auto it = lock_table_.begin(); it; it = lock_table_.next(it)) {
            const LockObject& obj = it->value;
            for (u32 i = 0; i < obj.requests.size(); i++) {
                if (!obj.requests[i].granted && obj.requests[i].txn_id == waiter) {
                    // This waiter is blocked on this lock object.
                    // Find all granted, incompatible holders.
                    for (u32 j = 0; j < obj.requests.size(); j++) {
                        if (obj.requests[j].granted &&
                            !is_compatible(obj.requests[j].mode, obj.requests[i].mode)) {
                            holders.push_back(obj.requests[j].txn_id);
                        }
                    }
                }
            }
        }
        return holders;
    };

    // DFS from txn_id, following wait-for edges
    HashMap<u64, bool> seen;
    Vector<u64> stack;
    stack.push_back(txn_id);

    while (!stack.empty()) {
        u64 current = stack.back();
        stack.pop_back();

        if (seen.contains(current)) return true;
        seen[current] = true;

        Vector<u64> holders = find_holder_for(current);
        for (u32 i = 0; i < holders.size(); i++) {
            if (holders[i] == txn_id) return true;  // Cycle found
            if (!seen.contains(holders[i])) {
                stack.push_back(holders[i]);
            }
        }
    }

    return false;
}

} // namespace minidb
