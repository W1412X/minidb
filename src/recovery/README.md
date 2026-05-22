# recovery

Write-ahead logging, crash recovery, and version garbage collection.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `WalManager` | `wal.h`, `wal.cpp` | WAL writer, group commit, fsync, replay, checkpoint barrier |
| `WalRecord` / `WalType` | `wal.h` | On-disk record header and type discriminator |
| `GarbageCollector` | `gc.h`, `gc.cpp` | Background DEAD-tuple pruning gated by the oldest active snapshot |

## Invariants

- **WAL-first** — every page write is preceded by
  `WalManager::flush_until(page_lsn)` (see `BufferPool::flush_frame_wal_first`).
  A persistent page never holds an LSN newer than durable WAL.
- **Commit durability** — `log_commit()` calls `flush_commit()` which is
  lost-wakeup safe via the `commit_batch_id_` counter. It returns `lsn`
  only when the record is on disk; on failure it returns 0 and the
  transaction layer rolls back.
- **Checkpoint barrier (D2)** — `WalManager::checkpoint(cb, ctx)` holds
  the WAL latch through all four phases: write `kCheckpoint`, fsync,
  invoke the callback (BufferPool flush + page-store fsync), truncate.
  No writer can sneak a record into the about-to-be-truncated window.
- **Recovery idempotency** — replay skips redo when `page_lsn >= record.lsn`.
  Index replay is currently a lazy rebuild via `Database::rebuild_all_indexes`;
  full physical index redo is tracked in `docs/ACID_TODO.md` A2.

## Recovery flow

1. `WalManager::recover(db)` scans `wal.log` and classifies every txn
   as committed / aborted from the kTxnCommit / kTxnAbort records.
2. Second pass: replay committed kInsert / kUpdate / kDelete records
   onto the heap (skipping when `page_lsn >= record.lsn`).
3. Undo phase: aborted txns get reversed via `heap->rollback_*`.
4. If any data record was replayed, the caller (`Database`) runs
   `rebuild_all_indexes()` so indexes match the recovered heap state.

Details in `docs/WAL_RECOVERY_PROTOCOL.md`.

## Tests

- `tests/unit/wal_buffer_pool_test.cpp` — WAL-first dirty-page flush.
- `tests/acid/atomicity/commit_durability.py` — A1/D3 commit durability.
- `tests/acid/durability/checkpoint_barrier.py` — D2 truncate-vs-flush race.
- `tests/acid/durability/torn_page.py` — D4 doublewrite + checksum.
- `tests/acid/durability/recovery_smoke.sh` — basic replay smoke.
- `tests/acid/durability/wal_replay_slot_reuse.sh` — replay with slot reuse.
- `tests/acid/durability/crash_recovery_harness.py` — randomised crash + idempotent recovery.
