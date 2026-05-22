/**
 * @file transaction.cpp
 * @brief MVCC Transaction Manager implementation
 */
#include "transaction/transaction.h"
#include "database/database.h"
#include "storage/heap_file.h"

namespace minidb {

thread_local Transaction* g_current_txn = nullptr;

Transaction::Transaction(u64 txn_id, u64 snapshot_id, Vector<u64>&& active_snapshot)
    : txn_id_(txn_id), snapshot_id_(snapshot_id),
      commit_id_(0), state_(TxnState::kActive),
      active_snapshot_(static_cast<Vector<u64>&&>(active_snapshot)),
      resource_acquired_(false) {}

void Transaction::record_insert(u32 table_id, const RecordId& rid) {
    UndoRecord rec;
    rec.type = UndoType::kInsert;
    rec.table_id = table_id;
    rec.rid = rid;
    undo_records_.push_back(rec);
}

void Transaction::record_delete(u32 table_id, const RecordId& rid) {
    UndoRecord rec;
    rec.type = UndoType::kDelete;
    rec.table_id = table_id;
    rec.rid = rid;
    undo_records_.push_back(rec);
}

void Transaction::record_hot_insert(u32 table_id, const RecordId& rid) {
    UndoRecord rec;
    rec.type = UndoType::kHotInsert;
    rec.table_id = table_id;
    rec.rid = rid;
    undo_records_.push_back(rec);
}

void Transaction::record_hot_delete(u32 table_id, const RecordId& rid) {
    UndoRecord rec;
    rec.type = UndoType::kHotDelete;
    rec.table_id = table_id;
    rec.rid = rid;
    undo_records_.push_back(rec);
}

TransactionManager::TransactionManager(Database* db)
    : db_(db), next_txn_id_(1) {
    u32 slots = db ? db->config().max_active_transactions : 256;
    if (slots == 0) slots = 1;
    txn_slots_.resize(slots);
}

Transaction* TransactionManager::current() const {
    return g_current_txn;
}

TxnSlot* TransactionManager::find_slot(u64 txn_id) {
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].txn_id == txn_id) {
            return &txn_slots_[i];
        }
    }
    return nullptr;
}

TxnSlot* TransactionManager::find_active_slot(u64 txn_id) {
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].txn_id == txn_id && txn_slots_[i].state == TxnState::kActive) {
            return &txn_slots_[i];
        }
    }
    return nullptr;
}

TxnSlot* TransactionManager::alloc_slot(u64 txn_id, u64 snapshot_id) {
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].txn_id == kInvalidTxnId || txn_slots_[i].state != TxnState::kActive) {
            txn_slots_[i].txn_id = txn_id;
            txn_slots_[i].snapshot_id = snapshot_id;
            txn_slots_[i].commit_id = 0;
            txn_slots_[i].state = TxnState::kActive;
            txn_slots_[i].home_page = kNullPageId;
            return &txn_slots_[i];
        }
    }
    return nullptr;
}

u64 TransactionManager::next_snapshot_id() {
    return next_txn_id_++;
}

Transaction* TransactionManager::begin() {
    if (g_current_txn) return nullptr;
    if (!db_->resources().acquire_transaction()) return nullptr;

    LockGuard guard(latch_);
    if (g_current_txn) {
        db_->resources().release_transaction();
        return nullptr;
    }

    TxnSlot* free_slot = nullptr;
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].txn_id == kInvalidTxnId || txn_slots_[i].state != TxnState::kActive) {
            free_slot = &txn_slots_[i];
            break;
        }
    }
    if (!free_slot) {
        db_->resources().release_transaction();
        return nullptr;
    }

    u64 txn_id = next_txn_id_++;
    u64 snapshot_id = next_txn_id_;  // Snapshot = next ID; all versions committed before snapshot_id are visible
    Vector<u64> active_snapshot;
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].txn_id != kInvalidTxnId && txn_slots_[i].state == TxnState::kActive) {
            active_snapshot.push_back(txn_slots_[i].txn_id);
        }
    }
    Transaction* txn = new Transaction(txn_id, snapshot_id,
                                       static_cast<Vector<u64>&&>(active_snapshot));
    txn->set_resource_acquired(true);
    free_slot->txn_id = txn_id;
    free_slot->snapshot_id = snapshot_id;
    free_slot->commit_id = 0;
    free_slot->state = TxnState::kActive;
    free_slot->home_page = kNullPageId;
    db_->wal().log_begin(txn_id);
    g_current_txn = txn;
    return txn;
}

bool TransactionManager::commit(Transaction* txn) {
    if (!txn || txn != g_current_txn) return false;

    u64 txn_id = txn->id();

    // Step 1: write the commit record and wait for it to become durable
    // BEFORE flipping the txn slot to kCommitted. This guarantees that
    // any reader who observes the committed state will also find the
    // commit record on disk after a crash. If durability fails, the
    // transaction is treated as aborted.
    u64 commit_lsn = db_->wal().log_commit(txn_id);
    if (commit_lsn == 0) {
        // WAL did not become durable. Drop back to the rollback path so
        // the slot/undo/lock state is unwound. rollback() also relies on
        // g_current_txn == txn, which is still true at this point.
        rollback(txn);
        return false;
    }

    // Step 2: the commit is durable. Publish it: assign commit id, flip
    // the slot to kCommitted, and capture the oldest active txn for GC
    // pruning. Doing this only now means a reader can never observe a
    // committed slot whose commit record is not yet on disk.
    latch_.lock();
    u64 commit_id = next_txn_id_++;
    u64 oldest_active = next_txn_id_;

    TxnSlot* slot = find_active_slot(txn_id);
    if (slot) {
        slot->commit_id = commit_id;
        slot->state = TxnState::kCommitted;
    }

    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].state == TxnState::kActive &&
            txn_slots_[i].txn_id != kInvalidTxnId &&
            txn_slots_[i].txn_id < oldest_active) {
            oldest_active = txn_slots_[i].txn_id;
        }
    }

    txn->set_commit_id(commit_id);
    txn->set_state(TxnState::kCommitted);
    latch_.unlock();

    // A1: append to the persistent xid → status log so a later restart
    // can still answer "did txn N commit?" after the slot is recycled.
    if (status_log_) status_log_->record(txn_id, TxnFinalState::kCommitted);

    const Vector<UndoRecord>& undo_records = txn->undo_records();
    for (u32 i = 0; i < undo_records.size(); i++) {
        const UndoRecord& rec = undo_records[i];
        if (rec.type != UndoType::kDelete && rec.type != UndoType::kHotDelete) continue;
        HeapFile* heap = db_->get_heap_file(rec.table_id);
        if (heap) {
            heap->prune_obsolete_version(rec.rid.page_id, rec.rid.slot_idx,
                                         oldest_active, txn_id, commit_lsn);
        }
    }

    db_->lock_manager().unlock_all(txn_id);

    bool release_resource = txn->resource_acquired();
    delete txn;
    g_current_txn = nullptr;
    if (release_resource) db_->resources().release_transaction();
    return true;
}

bool TransactionManager::rollback(Transaction* txn) {
    LockGuard guard(latch_);
    if (!txn || txn != g_current_txn) return false;

    TxnSlot* slot = find_slot(txn->id());
    if (slot) {
        slot->state = TxnState::kAborted;
    }

    u64 abort_lsn = db_->wal().log_abort(txn->id());

    // Free all locks held by the transaction
    db_->lock_manager().unlock_all(txn->id());

    const Vector<UndoRecord>& undo_records = txn->undo_records();
    for (i32 i = static_cast<i32>(undo_records.size()) - 1; i >= 0; i--) {
        const UndoRecord& rec = undo_records[static_cast<u32>(i)];
        HeapFile* heap = db_->get_heap_file(rec.table_id);
        if (!heap) continue;
        TableEntry* table = db_->catalog().get_table(rec.table_id);
        if (!table) continue;
        Tuple tuple;
        bool has_tuple = db_->read_tuple(rec.table_id, table->schema, rec.rid, &tuple);

        if (rec.type == UndoType::kInsert) {
            if (has_tuple) {
                db_->delete_index_entries(rec.table_id, tuple, rec.rid);
            }
            heap->rollback_insert(rec.rid.page_id, rec.rid.slot_idx, abort_lsn);
        } else if (rec.type == UndoType::kHotInsert) {
            heap->rollback_insert(rec.rid.page_id, rec.rid.slot_idx, abort_lsn);
        } else if (rec.type == UndoType::kDelete) {
            heap->rollback_delete(rec.rid.page_id, rec.rid.slot_idx, abort_lsn);
            if (has_tuple) {
                db_->insert_index_entries(rec.table_id, tuple, rec.rid);
            }
        } else if (rec.type == UndoType::kHotDelete) {
            heap->rollback_delete(rec.rid.page_id, rec.rid.slot_idx, abort_lsn);
        }
    }

    txn->set_state(TxnState::kAborted);
    u64 aborted_xid = txn->id();
    bool release_resource = txn->resource_acquired();
    delete txn;
    g_current_txn = nullptr;
    if (release_resource) db_->resources().release_transaction();
    // A1: persist the abort. Doing this AFTER the slot is freed means a
    // crash between rollback() returning and the next operation cannot lose
    // the fact that the txn was aborted, even if the slot has since been
    // reused.
    if (status_log_) status_log_->record(aborted_xid, TxnFinalState::kAborted);
    return true;
}

void TransactionManager::record_insert(u32 table_id, const RecordId& rid) {
    LockGuard guard(latch_);
    if (g_current_txn) {
        g_current_txn->record_insert(table_id, rid);
    }
}

void TransactionManager::record_delete(u32 table_id, const RecordId& rid) {
    LockGuard guard(latch_);
    if (g_current_txn) {
        g_current_txn->record_delete(table_id, rid);
    }
}

void TransactionManager::record_hot_insert(u32 table_id, const RecordId& rid) {
    LockGuard guard(latch_);
    if (g_current_txn) {
        g_current_txn->record_hot_insert(table_id, rid);
    }
}

void TransactionManager::record_hot_delete(u32 table_id, const RecordId& rid) {
    LockGuard guard(latch_);
    if (g_current_txn) {
        g_current_txn->record_hot_delete(table_id, rid);
    }
}

// ============================================================
// MVCC Visibility check — core algorithm
// ============================================================

bool TransactionManager::is_visible(u64 xmin, u64 xmax, const Transaction& txn) const {
    if (xmin == kInvalidTxnId) return false;

    auto was_active_in_snapshot = [&](u64 txn_id) -> bool {
        const Vector<u64>& snapshot = txn.active_snapshot();
        for (u32 i = 0; i < snapshot.size(); i++) {
            if (snapshot[i] == txn_id) return true;
        }
        return false;
    };

    // Rows inserted by self: visible (unless also deleted by self)
    if (xmin == txn.id()) {
        return (xmax != txn.id());
    }

    // Snapshot both xmin and xmax slot states under the latch to avoid
    // seeing a recycled slot between the two reads.
    TxnState xmin_state = TxnState::kActive;
    bool xmin_found = false;
    TxnState xmax_state = TxnState::kActive;
    bool xmax_found = false;

    {
        LockGuard guard(latch_);
        for (u32 i = 0; i < txn_slots_.size(); i++) {
            if (!xmin_found && txn_slots_[i].txn_id == xmin) {
                xmin_state = txn_slots_[i].state;
                xmin_found = true;
            }
            if (!xmax_found && xmax != kInvalidTxnId && txn_slots_[i].txn_id == xmax) {
                xmax_state = txn_slots_[i].state;
                xmax_found = true;
            }
            if (xmin_found && (xmax_found || xmax == kInvalidTxnId)) break;
        }
    }

    // Rule 1: xmin must be committed (not active and not aborted).
    if (xmin_found) {
        if (xmin_state == TxnState::kActive) return false;   // not committed
        if (xmin_state == TxnState::kAborted) return false;  // rolled back
    } else if (status_log_) {
        // Slot recycled: ask the persistent CLOG (A1). A recorded ABORTED
        // means we must not surface the tuple even though its xmin slot
        // is gone — closing the "recycled slot defaults to committed" hole.
        TxnFinalState s;
        if (status_log_->status(xmin, &s) && s == TxnFinalState::kAborted) {
            return false;
        }
    }
    // If the slot was recycled (txn_id reused by a newer txn) xmin_found is
    // false here and neither the CLOG nor the snapshot mark it aborted —
    // safe to treat as a long-since-committed transaction.
    if (was_active_in_snapshot(xmin)) return false;

    // Rule 1b: xmin must precede T's snapshot id.
    if (xmin >= txn.snapshot_id()) return false;

    // Rule 2: xmax checks.
    if (xmax == kInvalidTxnId) return true;  // not deleted

    if (xmax == txn.id()) return false;

    if (xmax_found) {
        if (xmax_state == TxnState::kActive) return true;   // delete not committed
        if (xmax_state == TxnState::kAborted) return true;  // delete rolled back -> treat as not deleted
    }
    // xmax not found → slot recycled, treat as not deleted

    if (was_active_in_snapshot(xmax)) return true;

    if (xmax >= txn.snapshot_id()) return true;  // delete committed after T started

    return false;  // deleted, and the delete is visible in T's snapshot
}

bool TransactionManager::is_txn_committed(u64 txn_id) const {
    LockGuard guard(latch_);
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].txn_id == txn_id) {
            return txn_slots_[i].state == TxnState::kCommitted;
        }
    }
    return false;
}

bool TransactionManager::get_txn_state(u64 txn_id, TxnState* out) const {
    if (!out) return false;
    LockGuard guard(latch_);
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].txn_id == txn_id) {
            *out = txn_slots_[i].state;
            return true;
        }
    }
    return false;
}

u64 TransactionManager::get_commit_id(u64 txn_id) const {
    LockGuard guard(latch_);
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].txn_id == txn_id) {
            return txn_slots_[i].commit_id;
        }
    }
    return 0;
}

u64 TransactionManager::get_oldest_active_txn_id() const {
    LockGuard guard(latch_);
    u64 oldest = next_txn_id_;
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].state == TxnState::kActive && txn_slots_[i].txn_id != kInvalidTxnId) {
            if (txn_slots_[i].txn_id < oldest) {
                oldest = txn_slots_[i].txn_id;
            }
        }
    }
    return oldest;
}

void TransactionManager::ensure_next_txn_id_at_least(u64 next_id) {
    LockGuard guard(latch_);
    if (next_id > next_txn_id_) {
        next_txn_id_ = next_id;
    }
}

u64 TransactionManager::next_txn_id() const {
    LockGuard guard(latch_);
    return next_txn_id_;
}

void TransactionManager::set_status_log(UniquePtr<TxnStatusLog> log) {
    status_log_ = static_cast<UniquePtr<TxnStatusLog>&&>(log);
}

} // namespace minidb
