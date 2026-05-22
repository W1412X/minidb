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
      isolation_(IsolationLevel::kSnapshot),
      resource_acquired_(false) {}

void Transaction::record_read(u32 table_id, const RecordId& rid) {
    if (isolation_ != IsolationLevel::kSerializable) return;
    // Cheap de-dup: the same row scanned twice in one statement (or once
    // per re-scan in a join) should not bloat the read set unbounded.
    // Linear scan is fine because read sets stay small in practice; if
    // they grow, a hash set can swap in later.
    for (u32 i = 0; i < read_set_.size(); i++) {
        if (read_set_[i].table_id == table_id &&
            read_set_[i].rid == rid) {
            return;
        }
    }
    ReadRecord rec;
    rec.table_id = table_id;
    rec.rid = rid;
    read_set_.push_back(rec);
}

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

void Transaction::record_ddl(UndoType type, u32 table_id, DdlUndoInfo&& info) {
    u32 idx = ddl_undo_infos_.size();
    ddl_undo_infos_.push_back(static_cast<DdlUndoInfo&&>(info));
    UndoRecord rec;
    rec.type = type;
    rec.table_id = table_id;
    rec.rid = RecordId();
    rec.ddl_info_idx = idx;
    undo_records_.push_back(rec);
}

void Transaction::truncate_undo(u32 mark) {
    // Count DDL undo infos referenced past the mark so we can
    // truncate the parallel ddl_undo_infos_ vector in sync.
    u32 ddl_remove = 0;
    for (u32 i = mark; i < undo_records_.size(); i++) {
        if (static_cast<u8>(undo_records_[i].type) >= 10) {
            ddl_remove++;
        }
    }
    while (undo_records_.size() > mark) {
        undo_records_.pop_back();
    }
    while (ddl_remove > 0 && !ddl_undo_infos_.empty()) {
        ddl_undo_infos_.pop_back();
        ddl_remove--;
    }
}

TransactionManager::TransactionManager(Database* db)
    : db_(db), next_txn_id_(1),
      default_isolation_(IsolationLevel::kSnapshot) {
    u32 slots = db ? db->config().max_active_transactions : 256;
    if (slots == 0) slots = 1;
    txn_slots_.resize(slots);
}

void TransactionManager::set_default_isolation(IsolationLevel level) {
    LockGuard guard(latch_);
    default_isolation_ = level;
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

    // Single pass: capture both the first free slot AND any currently active
    // txn ids for our snapshot. The old code walked the slot array twice —
    // once for each. Doing it in one pass halves the per-begin() work on the
    // autocommit hot path, where 256 slots are mostly empty.
    TxnSlot* free_slot = nullptr;
    Vector<u64> active_snapshot;
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        const TxnSlot& s = txn_slots_[i];
        if (s.txn_id == kInvalidTxnId || s.state != TxnState::kActive) {
            if (!free_slot) free_slot = &txn_slots_[i];
            continue;
        }
        active_snapshot.push_back(s.txn_id);
    }
    if (!free_slot) {
        db_->resources().release_transaction();
        return nullptr;
    }

    u64 txn_id = next_txn_id_++;
    u64 snapshot_id = next_txn_id_;  // Snapshot = next ID; all versions committed before snapshot_id are visible
    Transaction* txn = new Transaction(txn_id, snapshot_id,
                                       static_cast<Vector<u64>&&>(active_snapshot));
    txn->set_resource_acquired(true);
    txn->set_isolation(default_isolation_);
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

    // SSI-lite (I3) — only for serializable transactions. Detect rw-
    // conflict against any transaction that committed during our lifetime
    // by intersecting our read set with their write set. If any overlap
    // exists, we must abort to break a potential serialization cycle.
    // This rejects more schedules than strict SSI (which only aborts
    // dangerous-structure cycles), but it is provably serializable: if
    // every conflicting reader aborts, the surviving txns serialize in
    // commit order. Done BEFORE writing the commit record so an aborted
    // serializable txn cleans up exactly like any other rollback.
    if (txn->isolation() == IsolationLevel::kSerializable &&
        ssi_check_conflict(*txn)) {
        rollback(txn);
        return false;
    }

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

    // SSI bookkeeping: any concurrent serializable txn now in flight may
    // need to see our write set at its own commit time. Record a compact
    // (table_id, rid) entry for every kInsert / kDelete / kHotInsert /
    // kHotDelete undo. We also prune entries that no live txn could
    // serialise against anymore, so memory stays bounded.
    {
        CommittedWriteSet entry;
        entry.commit_id = commit_id;
        entry.txn_id = txn_id;
        const Vector<UndoRecord>& undos = txn->undo_records();
        for (u32 i = 0; i < undos.size(); i++) {
            ReadRecord w;
            w.table_id = undos[i].table_id;
            w.rid = undos[i].rid;
            entry.writes.push_back(w);
        }
        committed_history_.push_back(static_cast<CommittedWriteSet&&>(entry));
        prune_committed_history();
    }
    latch_.unlock();

    // Transactional DDL: delete files that were deferred by DROP
    // TABLE / DROP INDEX during this transaction. Now that the txn is
    // committed, the catalog changes are final and the physical files
    // can be removed.
    if (!txn->ddl_undo_infos().empty()) {
        db_->commit_ddl_deferred(txn->ddl_undo_infos());
    }

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

// Apply one undo record. Shared by full rollback and savepoint rollback.
// `abort_lsn` is stamped on touched pages. `for_savepoint` chooses whether
// a compensating WAL record is emitted: full rollback already writes
// log_abort and recovery skips the whole transaction, so no per-record
// compensation is needed there. Statement-level savepoint, however, will
// commit later — recovery must be told to undo those specific records.
static void apply_undo_record(Database* db, const UndoRecord& rec, u64 abort_lsn,
                              bool for_savepoint, u64 txn_id,
                              const DdlUndoInfo* ddl_info = nullptr) {
    // DDL undo types — reverse the schema change.
    if (static_cast<u8>(rec.type) >= 10) {
        if (!ddl_info) return;
        switch (rec.type) {
            case UndoType::kDdlCreateTable:
                db->undo_create_table(rec.table_id, *ddl_info);
                break;
            case UndoType::kDdlDropTable:
                db->undo_drop_table(rec.table_id, *ddl_info);
                break;
            case UndoType::kDdlCreateIndex:
                db->undo_create_index(rec.table_id, *ddl_info);
                break;
            case UndoType::kDdlDropIndex:
                db->undo_drop_index(rec.table_id, *ddl_info);
                break;
            case UndoType::kDdlAlterAddColumn:
                db->undo_alter_add_column(rec.table_id, *ddl_info);
                break;
            case UndoType::kDdlAlterDropColumn:
                db->undo_alter_drop_column(rec.table_id, *ddl_info);
                break;
            case UndoType::kDdlAlterRenameColumn:
                db->undo_alter_rename_column(rec.table_id, *ddl_info);
                break;
            default:
                break;
        }
        return;
    }

    HeapFile* heap = db->get_heap_file(rec.table_id);
    if (!heap) return;
    TableEntry* table = db->catalog().get_table(rec.table_id);
    if (!table) return;
    Tuple tuple;
    bool has_tuple = db->read_tuple(rec.table_id, table->schema, rec.rid, &tuple);

    if (rec.type == UndoType::kInsert) {
        if (for_savepoint) {
            db->wal().log_savepoint_undo_insert(txn_id, rec.table_id,
                                                rec.rid.page_id, rec.rid.slot_idx);
        }
        if (has_tuple) {
            db->delete_index_entries(rec.table_id, tuple, rec.rid);
        }
        heap->rollback_insert(rec.rid.page_id, rec.rid.slot_idx, abort_lsn);
    } else if (rec.type == UndoType::kHotInsert) {
        if (for_savepoint) {
            db->wal().log_savepoint_undo_insert(txn_id, rec.table_id,
                                                rec.rid.page_id, rec.rid.slot_idx);
        }
        heap->rollback_insert(rec.rid.page_id, rec.rid.slot_idx, abort_lsn);
    } else if (rec.type == UndoType::kDelete) {
        if (for_savepoint) {
            db->wal().log_savepoint_undo_delete(txn_id, rec.table_id,
                                                rec.rid.page_id, rec.rid.slot_idx);
        }
        heap->rollback_delete(rec.rid.page_id, rec.rid.slot_idx, abort_lsn);
        // No re-insert needed: under lazy index cleanup the entry is still
        // there. Clearing xmax restores visibility.
    } else if (rec.type == UndoType::kHotDelete) {
        if (for_savepoint) {
            db->wal().log_savepoint_undo_delete(txn_id, rec.table_id,
                                                rec.rid.page_id, rec.rid.slot_idx);
        }
        heap->rollback_delete(rec.rid.page_id, rec.rid.slot_idx, abort_lsn);
    }
}

bool TransactionManager::rollback(Transaction* txn) {
    if (!txn || txn != g_current_txn) return false;

    // Phase 1 (under latch): flip slot to kAborted so concurrent readers
    // immediately see this txn as aborted. Keep the latch narrow — never
    // hold it while doing heap/index/catalog I/O (avoids deadlock with
    // checkpoint, background threads, and other txn lifecycle calls).
    {
        LockGuard guard(latch_);
        TxnSlot* slot = find_slot(txn->id());
        if (slot) {
            slot->state = TxnState::kAborted;
        }
    }

    u64 abort_lsn = db_->wal().log_abort(txn->id());

    // Free all locks held by the transaction
    db_->lock_manager().unlock_all(txn->id());

    // Phase 2 (latch released): apply undo records. These do heap I/O,
    // index mutations, and catalog persistence — all operations that may
    // acquire other latches (page latch, WAL latch, catalog file I/O).
    const Vector<UndoRecord>& undo_records = txn->undo_records();
    for (i32 i = static_cast<i32>(undo_records.size()) - 1; i >= 0; i--) {
        const UndoRecord& rec = undo_records[static_cast<u32>(i)];
        const DdlUndoInfo* ddl_info = nullptr;
        if (static_cast<u8>(rec.type) >= 10) {
            ddl_info = &txn->ddl_undo_infos()[rec.ddl_info_idx];
        }
        apply_undo_record(db_, rec, abort_lsn,
                          /*for_savepoint=*/false, txn->id(), ddl_info);
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

bool TransactionManager::rollback_to_savepoint(Transaction* txn, u32 mark) {
    if (!txn || txn != g_current_txn) return false;
    const Vector<UndoRecord>& undo = txn->undo_records();
    if (mark >= undo.size()) return true;     // nothing to undo

    // Apply in reverse, just like full rollback. No log_abort and no lock
    // release — the transaction stays ACTIVE. abort_lsn=0 keeps the heap
    // pages' LSNs unchanged (they have not crossed any commit boundary
    // yet). No latch needed: g_current_txn is thread-local and the txn is
    // kActive, so only this thread manipulates its undo log.
    for (i32 i = static_cast<i32>(undo.size()) - 1; i >= static_cast<i32>(mark); i--) {
        const UndoRecord& rec = undo[static_cast<u32>(i)];
        const DdlUndoInfo* ddl_info = nullptr;
        if (static_cast<u8>(rec.type) >= 10) {
            ddl_info = &txn->ddl_undo_infos()[rec.ddl_info_idx];
        }
        apply_undo_record(db_, rec, 0,
                          /*for_savepoint=*/true, txn->id(), ddl_info);
    }
    txn->truncate_undo(mark);
    return true;
}

void TransactionManager::record_insert(u32 table_id, const RecordId& rid) {
    // g_current_txn is thread_local — only the owning thread reads/writes it.
    // No latch needed; removing it eliminates contention with background GC
    // threads that acquire latch_ for is_txn_committed() lookups.
    if (g_current_txn) {
        g_current_txn->record_insert(table_id, rid);
    }
}

void TransactionManager::record_delete(u32 table_id, const RecordId& rid) {
    if (g_current_txn) {
        g_current_txn->record_delete(table_id, rid);
    }
}

void TransactionManager::record_hot_insert(u32 table_id, const RecordId& rid) {
    if (g_current_txn) {
        g_current_txn->record_hot_insert(table_id, rid);
    }
}

void TransactionManager::record_hot_delete(u32 table_id, const RecordId& rid) {
    if (g_current_txn) {
        g_current_txn->record_hot_delete(table_id, rid);
    }
}

// ============================================================
// MVCC Visibility check — core algorithm
// ============================================================

bool TransactionManager::is_visible(u64 xmin, u64 xmax, const Transaction& txn) const {
    // --- Cheap fast-path checks (no latch) ----------------------------------
    if (xmin == kInvalidTxnId) return false;
    if (xmin == kFrozenTxnId) return xmax == kInvalidTxnId;
    if (xmin == txn.id()) return (xmax != txn.id());

    auto was_active_in_snapshot = [&](u64 txn_id) -> bool {
        // active_snapshot is normally small (≤ #concurrent txns), so a linear
        // scan beats hashing for the common 1-10 entry case.
        const Vector<u64>& snapshot = txn.active_snapshot();
        for (u32 i = 0; i < snapshot.size(); i++) {
            if (snapshot[i] == txn_id) return true;
        }
        return false;
    };

    // --- Hot path: xmin precedes our snapshot --------------------------------
    // When xmin < snapshot_id, the transaction was already either active or
    // finished at our begin() time. The active set we captured then is
    // authoritative — no live slot lookup is required. status_log answers any
    // "was it aborted?" question without holding the manager latch.
    //
    // This removes the global latch + O(#slots) linear scan that was hit on
    // every visible-tuple test, which was the single biggest serialization
    // point during scans/lookups (5000 rows × ≥1 latch each).
    if (xmin < txn.snapshot_id()) {
        if (was_active_in_snapshot(xmin)) return false;
        if (status_log_) {
            TxnFinalState s;
            if (status_log_->status(xmin, &s) && s == TxnFinalState::kAborted) {
                return false;
            }
        }
        // xmin is committed from our snapshot's perspective.
        if (xmax == kInvalidTxnId) return true;
        if (xmax == txn.id()) return false;
        if (xmax >= txn.snapshot_id()) return true;        // delete after we began
        if (was_active_in_snapshot(xmax)) return true;     // delete still pending
        if (status_log_) {
            TxnFinalState xs;
            if (status_log_->status(xmax, &xs) && xs == TxnFinalState::kAborted) {
                return true;                               // delete rolled back
            }
        }
        return false;                                       // delete committed
    }

    // --- xmin >= snapshot_id: definitely not visible to us. -----------------
    // (Future txns can't be visible to an older snapshot under SI.)
    return false;
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

// Walk every committed-history entry whose commit_id > our snapshot_id
// (i.e. transactions that committed while we were running, plus our own
// concurrent set since slot allocation) and check whether any of their
// write entries collide with a read we recorded. Latch must NOT be held
// — caller hits this BEFORE the publish step that takes the latch.
bool TransactionManager::ssi_check_conflict(const Transaction& txn) const {
    const Vector<ReadRecord>& reads = txn.read_set();
    if (reads.size() == 0) return false;
    u64 snapshot = txn.snapshot_id();
    LockGuard guard(latch_);
    for (u32 i = 0; i < committed_history_.size(); i++) {
        const CommittedWriteSet& entry = committed_history_[i];
        if (entry.txn_id == txn.id()) continue;        // ignore our own
        if (entry.commit_id < snapshot) continue;      // committed before us; not concurrent
        for (u32 w = 0; w < entry.writes.size(); w++) {
            const ReadRecord& wr = entry.writes[w];
            for (u32 r = 0; r < reads.size(); r++) {
                if (reads[r].table_id == wr.table_id &&
                    reads[r].rid == wr.rid) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Drop history entries that no live transaction could still need. An
// entry with commit_id C matters only while some active txn has
// snapshot_id <= C; once every active snapshot has moved past C the
// entry can never participate in another conflict check. Linear pass —
// committed_history_ stays short in practice because txns commit/abort
// promptly. Latch must be held by caller.
void TransactionManager::prune_committed_history() {
    u64 oldest_snapshot = next_txn_id_;
    bool any_active = false;
    for (u32 i = 0; i < txn_slots_.size(); i++) {
        if (txn_slots_[i].state == TxnState::kActive &&
            txn_slots_[i].txn_id != kInvalidTxnId) {
            any_active = true;
            if (txn_slots_[i].snapshot_id < oldest_snapshot) {
                oldest_snapshot = txn_slots_[i].snapshot_id;
            }
        }
    }
    if (!any_active) {
        committed_history_.clear();
        return;
    }
    Vector<CommittedWriteSet> kept;
    for (u32 i = 0; i < committed_history_.size(); i++) {
        if (committed_history_[i].commit_id >= oldest_snapshot) {
            kept.push_back(static_cast<CommittedWriteSet&&>(committed_history_[i]));
        }
    }
    committed_history_ = static_cast<Vector<CommittedWriteSet>&&>(kept);
}

} // namespace minidb
