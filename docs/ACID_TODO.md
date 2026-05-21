# MiniDB ACID Hardening TODO

This document captures the concrete gaps between MiniDB's current implementation
and a strict ACID database, together with the test items that must accompany
each fix. It is meant to be the single source of truth for "what does it take
to call MiniDB ACID-compliant".

It is organised by ACID property (A / C / I / D), with each item carrying:

- **Goal** — invariant we want to hold.
- **Current state** — what the code does today, with file:line references.
- **Action items** — concrete code changes required.
- **Tests** — automated tests that must be added (script name + assertion).

Priorities: **P0 = correctness-blocker**, **P1 = correctness-hardening**,
**P2 = nice-to-have / SQL-semantics**.

---

## A — Atomicity

### A1. Crash mid-commit must yield all-or-nothing [P0] — **DONE**

Slot flip moved after `flush_commit` succeeds; group-commit follower
loop is now lost-wakeup safe via a per-batch generation counter; fsync
failure returns 0 from `log_commit` and `TransactionManager::commit`
falls back to the rollback path. Test:
`tests/acid/atomicity/commit_durability.py`.

---


- **Goal**: A committed txn is visible after restart iff its commit WAL record
  is durable; otherwise the txn must be rolled back during replay.
- **Current state**:
  - `TransactionManager::commit` writes the slot to `kCommitted` **before**
    fsync ([src/transaction/transaction.cpp:155-174](../src/transaction/transaction.cpp)).
    If we crash between `set_state(kCommitted)` and `wal.flush()`, the in-memory
    slot is gone, but readers in the same process briefly saw a "committed"
    state. On recovery the txn looks aborted (no commit record durable), which
    is correct on disk but inconsistent with the brief in-memory window.
  - Group commit waiters use `commit_cond_` ([src/recovery/wal.cpp](../src/recovery/wal.cpp))
    but the follower path returns the LSN without re-checking `durable_lsn_`
    after a spurious wakeup or timeout.
- **Action items**:
  - Flip the order: only mark the slot `kCommitted` **after** `flush_commit`
    succeeds; if fsync fails, mark `kAborted` and run the rollback path.
  - In `flush_commit`, loop on the condition variable until
    `durable_lsn_ >= commit_lsn` (lost-wakeup safe).
  - Surface fsync errors back to the user as a hard `ERROR` instead of returning
    silently.
- **Tests**:
  - `tests/acid/atomicity/commit_fsync_failure.sh` — inject `wal_fsync = off`
    plus `SIGKILL` immediately after `COMMIT;` returns. After restart, neither
    half-committed rows nor "ghost commits" are visible.
  - `tests/acid/atomicity/group_commit_wait.py` — N concurrent commits,
    verify every successful `COMMIT;` reply implies `durable_lsn >= its lsn`.

### A2. Index entry / heap tuple atomicity [P0]

- **Goal**: For every committed heap tuple, all matching index entries exist;
  for every rolled-back tuple, no index entries leak.
- **Current state**:
  - Insert path performs `heap.insert` then `index.insert`
    ([src/database/database.cpp](../src/database/database.cpp)); if the index
    insert fails (e.g. OOM, duplicate), the heap tuple is left behind.
  - Recovery sets `needs_index_rebuild = true` and does a lazy rebuild
    ([src/recovery/wal.cpp](../src/recovery/wal.cpp)); this is documented as
    incomplete in
    [CAPABILITY_GAP_CHECKLIST.md:61-65](CAPABILITY_GAP_CHECKLIST.md).
- **Action items**:
  - Decide redo vs rebuild and pick one. Recommendation: keep lazy rebuild for
    `CREATE INDEX`, but require physical redo for all `INSERT/UPDATE/DELETE`
    index WAL records during normal recovery.
  - Make index WAL replay idempotent (skip if key + RID already present).
  - On insert failure mid-row, roll back the heap insert before returning the
    error.
- **Tests**:
  - `tests/acid/atomicity/index_heap_consistency.py` — random
    INSERT/UPDATE/DELETE workload, after each `COMMIT` assert
    `heap_scan(table) == index_scan(table)` on every index.
  - Extend `tests/storage/recovery/recovery_smoke.sh` to cover crash between
    `heap.insert` and `index.insert` (use a `MINIDB_FAULT=index_insert_skip`
    debug knob to be added).

### A3. Rollback must restore unique-key reservation [P1]

- **Goal**: `BEGIN; INSERT k=1; ROLLBACK;` followed by `INSERT k=1` succeeds.
- **Current state**: `tuple_live_for_unique_check` in
  [src/sql/executor/insert.cpp:82+](../src/sql/executor/insert.cpp) already
  checks MVCC liveness, but the index entry from the rolled-back insert is
  removed only in `rollback()` via `delete_index_entries`. Crash before
  rollback completes leaks the index entry.
- **Action items**: WAL-log unique-key reservations the same way as heap
  inserts; recovery must replay the corresponding delete for aborted txns.
- **Tests**:
  - `tests/acid/atomicity/unique_rollback_crash.sh` — kill process between
    `INSERT` and `ROLLBACK`, then on restart try the same key, must succeed.

### A4. DDL atomicity [P1]

- **Goal**: `ALTER TABLE … ADD/DROP COLUMN` is all-or-nothing across crashes.
- **Current state**: ALTER TABLE rewrites tuples in place
  ([src/database/database.cpp:448+](../src/database/database.cpp)); no WAL
  record covers schema version transitions, so a crash mid-rewrite can leave
  tuples with the new layout and the catalog with the old schema.
- **Action items**: write a `kDDLBegin` / `kDDLCommit` WAL pair, log
  per-tuple rewrite records, and refuse to load a catalog whose schema version
  has no matching `kDDLCommit`.
- **Tests**:
  - `tests/acid/atomicity/ddl_crash.sh` — kill between ALTER tuple rewrite
    and catalog flush; restart must either show old schema or new schema, never
    a mix.

---

## C — Consistency

### C1. Enforce NOT NULL / CHECK / DEFAULT at DML layer [P0] — **PARTIAL**

- NOT NULL: now surfaces as `Error: NOT NULL constraint violated` on
  both INSERT and UPDATE instead of silently skipping the row.
- DEFAULT: `CREATE TABLE` and `ALTER TABLE` accept it; the planner
  substitutes the column default for any column omitted from an INSERT
  column list. `Column::default_as_value()` is the single source of
  truth for parsing the textual default into a typed Value.
- CHECK: still unimplemented.

Test: `tests/acid/consistency/constraints_not_null_default.py`.

---


- **Goal**: Constraints declared in DDL hold for every committed row.
- **Current state**:
  - `InsertExecutor::row_satisfies_schema` rejects NOT NULL violations
    ([src/sql/executor/insert.cpp:71-80](../src/sql/executor/insert.cpp)).
  - `UpdateExecutor` does **not** re-check NOT NULL after applying the SET list.
  - No `CHECK` constraint parser/storage at all.
  - `DEFAULT` is applied only in `ALTER TABLE ADD COLUMN`
    ([src/database/database.cpp:448](../src/database/database.cpp)); INSERT
    with missing columns rejects the row instead of substituting the default.
- **Action items**:
  - Add NOT NULL post-condition check in `UpdateExecutor`.
  - Add `Column::check_expr` storage + evaluator and wire it into INSERT and
    UPDATE.
  - Substitute `DEFAULT` for omitted columns on INSERT.
- **Tests**:
  - `tests/acid/consistency/constraints_not_null.sh`
  - `tests/acid/consistency/constraints_check.sh`
  - `tests/acid/consistency/constraints_default.sh`

### C2. Unique / PK race under concurrency [P0]

- **Goal**: Two concurrent INSERTs with the same unique key must conflict
  deterministically — exactly one succeeds.
- **Current state**: `InsertExecutor::violates_unique_constraints` is read-then-
  insert without a key-level lock; the index B+ tree uses coarse tree locking
  but does not block other writers from observing the same "free" key window.
  Lock manager exposes `lock_key` ([src/concurrency/lock_manager.h:69](../src/concurrency/lock_manager.h))
  but insert does not call it for unique keys.
- **Action items**:
  - Take `lock_key(txn, table, encoded_key, kExclusive)` for every unique key
    before the index lookup; release on commit/abort.
  - Document this in `docs/CONCURRENCY_CONTROL.md`.
- **Tests**:
  - `tests/acid/isolation/unique_insert_race.py` — N threads insert the same
    key, assert exactly one OK and N-1 unique-violation errors, no orphans.

### C3. Foreign keys (deferred) [P2]

- **Goal**: Referential integrity if/when `FOREIGN KEY` syntax is parsed.
- **Current state**: Not parsed.
- **Action items**: Out of scope for ACID baseline. Document in
  `docs/KNOWN_LIMITATIONS.md` that FK is unsupported.

### C4. Post-recovery consistency assertion [P0]

- **Goal**: On clean startup, MiniDB asserts heap/index/catalog agreement.
- **Current state**: Recovery rebuilds indexes lazily; no consistency
  assertion runs.
- **Action items**:
  - Add a `--verify` startup flag that runs the existing consistency checker
    over every table and refuses to open the DB if it finds mismatches.
  - Run it automatically when `MINIDB_VERIFY=on`.
- **Tests**:
  - `tests/acid/consistency/post_recovery_verify.sh` — random crash workload,
    on restart `--verify` must always pass.

---

## I — Isolation

### I1. Snapshot isolation correctness [P0]

- **Goal**: A read sees exactly one consistent snapshot for its lifetime;
  read-own-write, read-own-delete, and old-snapshot rules hold.
- **Current state**: `TransactionManager::is_visible`
  ([src/transaction/transaction.cpp:278-346](../src/transaction/transaction.cpp))
  implements snapshot isolation but reads xmin/xmax slot states under the
  latch. Slot recycling is handled by re-checking `txn_id`, which is correct
  only if `next_txn_id_` never wraps.
- **Action items**:
  - Add a wraparound watchdog: refuse new transactions when
    `next_txn_id_ > 2^62`.
  - Add explicit "slot generation" counter to defend against ABA on slot
    reuse.
- **Tests**:
  - `tests/acid/isolation/snapshot_matrix.py` — full visibility matrix for
    {insert, update, delete} × {commit, rollback} × {own, other, old-snapshot}.
  - `tests/acid/isolation/long_snapshot_gc.py` — long reader keeps an old
    snapshot, writers commit many versions, reader still sees the original
    snapshot and GC does not prune visible versions.

### I2. Lost update prevention [P0] — **DONE**

UpdateExecutor and DeleteExecutor already took a per-row `RowExclusive`
lock and re-read `xmax` after acquiring it. The conflict was silently
swallowed with `continue`; now both paths surface
`Error: could not serialize access due to concurrent update` and abort
the statement. Test: `tests/acid/isolation/lost_update.py`.

---


- **Goal**: Two concurrent transactions updating the same row must not silently
  lose one update.
- **Current state**: Lock manager has `lock_record`, but Update path takes
  only a table-level `RowExclusive` lock; on conflicting MVCC writes, the
  second writer sees the unchanged version and overwrites it.
- **Action items**:
  - On UPDATE/DELETE, acquire `lock_record(txn, table, rid, kExclusive)`
    before writing.
  - If the row's xmax already names a different active txn, raise a
    serialization error.
- **Tests**:
  - `tests/acid/isolation/lost_update.py` — two txns SET v=v+1 on the same row;
    after both commit, v must be incremented by 2, never 1.

### I3. Phantom prevention for SERIALIZABLE [P1] — **DONE (docs only)**

MiniDB is documented as SI, not SERIALIZABLE
(`docs/CONCURRENCY_CONTROL.md`). The parser refuses
`SET TRANSACTION ISOLATION LEVEL`, so clients cannot silently downgrade.
`tests/acid/isolation/write_skew.py` is a regression test that pins the
documented SI write-skew behaviour: when MiniDB later grows SSI, this
test should be flipped to expect an abort. Implementing real SSI is
deferred.

---


- **Goal**: Range predicates do not see new rows inserted by concurrent txns.
- **Current state**: No predicate locks, no gap locks; MiniDB advertises
  snapshot isolation only.
- **Action items**:
  - Either (a) document explicitly that MiniDB offers SI not SERIALIZABLE and
    refuse `SET TRANSACTION ISOLATION LEVEL SERIALIZABLE`, or (b) implement
    SSI / range locks.
  - Recommendation for now: (a) — update README to say "snapshot isolation,
    not serializable".
- **Tests**:
  - `tests/acid/isolation/write_skew.py` — classic doctor-on-call write-skew
    case; assert MiniDB reproduces the SI anomaly and document the fact.

### I4. Deadlock detection fairness [P1]

- **Goal**: Long-running transactions are not perpetually starved.
- **Current state**: Wait-for graph cycle detector picks the **youngest** txn
  as the victim ([src/concurrency/lock_manager.cpp](../src/concurrency/lock_manager.cpp)),
  which can starve old transactions if they keep reconflicting with younger ones.
- **Action items**: switch to "oldest-survives, youngest-aborts" or
  "wound-wait" selection.
- **Tests**:
  - `tests/acid/isolation/deadlock_fairness.py` — three-cycle deadlock, the
    longest-running txn must survive.

### I5. Lock release guarantees [P1]

- **Goal**: All locks held by a txn are released at COMMIT, ROLLBACK, process
  exit, or connection drop.
- **Current state**: `unlock_all(txn_id)` is called on commit and rollback
  ([src/transaction/transaction.cpp:188,209](../src/transaction/transaction.cpp));
  connection close is not exercised by tests.
- **Action items**:
  - Hook `TcpServer::on_disconnect` into `txn_mgr.rollback(current)`.
  - Add an idle-txn reaper that rolls back txns whose owning connection has
    been gone > `idle_in_transaction_timeout`.
- **Tests**:
  - `tests/acid/isolation/connection_drop_release.py` — drop the TCP socket
    mid-transaction, verify the table lock is released and a new connection
    can immediately acquire it.

---

## D — Durability

### D1. WAL-first ordering for every page write [P0]

- **Goal**: A page on disk never has `page_lsn > durable_wal_lsn`.
- **Current state**: `BufferPool::flush_page` calls
  `wal.flush_until(page_lsn)` before write
  ([src/storage/buffer_pool.cpp](../src/storage/buffer_pool.cpp)). Checkpoint
  truncation does not assert this invariant globally.
- **Action items**:
  - At checkpoint, scan all dirty frames; if any `page_lsn > durable_lsn`,
    flush WAL again before truncating.
  - Add an assertion in debug builds.
- **Tests**:
  - `tests/acid/durability/wal_first_invariant.cpp` — extends
    `wal_buffer_pool_test` to cover all pages flushed by checkpoint, not just
    eviction.

### D2. Checkpoint barrier [P0]

- **Goal**: WAL truncation never drops redo records still needed by unflushed
  pages.
- **Current state**: `checkpoint()` calls `wal_->checkpoint()` then truncates
  ([src/recovery/wal.cpp](../src/recovery/wal.cpp)); does not block in-flight
  writers nor force flush all dirty pages.
- **Action items**:
  - Make checkpoint a 3-phase protocol: (1) freeze new dirty-page allocations
    behind a global checkpoint latch, (2) flush all dirty pages, (3) fsync
    WAL, then truncate.
- **Tests**:
  - `tests/acid/durability/checkpoint_no_data_loss.py` — heavy writers in
    background, force `CHECKPOINT`, then `SIGKILL` immediately; on restart no
    committed row is missing.

### D3. Group-commit correctness [P0] — **DONE**

Folded into A1; see test `tests/acid/atomicity/commit_durability.py`.

---


- **Goal**: Every commit returns only after its commit LSN is durable.
- **Current state**: see A1.
- **Action items**: covered by A1.
- **Tests**: covered by A1 + a stress test
  `tests/acid/durability/group_commit_stress.py`.

### D4. Torn-page protection [P0] — **DONE (defaults + regression)**

`DbConfig` already defaults `doublewrite=on` and `page_checksum=on`.
`DiskManager` writes the doublewrite copy with checksum + magic before
overwriting the main page, and `recover_double_write()` restores from
it on startup. `tests/acid/durability/torn_page.py` covers both
invariants: corrupted page never surfaces as garbage rows, and a
synthesized `doublewrite.bin` restores a torn page.

---


- **Goal**: A power loss in the middle of writing an 8KB page is recoverable.
- **Current state**:
  - Doublewrite is implemented and recovered at startup
    ([src/storage/disk_manager.cpp:51-225](../src/storage/disk_manager.cpp)).
  - Page checksum is verified on read when `page_checksum = on`.
  - **Gap**: doublewrite default is config-driven; remote `PageStore` path
    does not use it.
- **Action items**:
  - Default `doublewrite = on` and `page_checksum = on` in
    `src/common/db_config.cpp`.
  - Mirror doublewrite into the remote write path or document that remote
    storage is responsible for atomic page writes (it currently is, via the
    PageServer's own buffer protocol).
- **Tests**:
  - `tests/acid/durability/torn_page.cpp` — write a page, truncate the file
    to 4KB, restart, verify the doublewrite copy restores the page.

### D5. Recovery idempotency [P0]

- **Goal**: Running recovery twice on the same WAL produces identical state.
- **Current state**: Page-LSN checks (`if page_lsn >= record_lsn skip`) make
  most redo idempotent; index redo currently relies on lazy rebuild (see A2).
- **Action items**:
  - After A2 lands, write a unit test that calls `WalManager::recover` twice
    and diffs every page.
- **Tests**:
  - `tests/acid/durability/recovery_idempotency.cpp` (new C++ test that
    invokes the recovery path directly).
  - Extend `tests/storage/recovery/crash_recovery_harness.py` (already does
    3 idempotent passes; add an explicit assertion that the WAL durable LSN
    is the same after each pass).

### D6. fsync error handling [P1]

- **Goal**: An fsync failure is treated as a hard error; we never silently
  continue.
- **Current state**: `disk_manager.cpp` writes/fsyncs without surfacing errors
  to higher layers in some paths.
- **Action items**: thread `Result<void>` through `flush_page` and `fsync`,
  panic if a previously-acked commit can't be recovered.
- **Tests**:
  - `tests/acid/durability/fsync_failure.sh` — use `LD_PRELOAD` shim to make
    `fsync()` return EIO; verify subsequent commits fail loudly.

---

## Cross-cutting

### X1. Test taxonomy

All new ACID tests go under `tests/acid/{atomicity,consistency,isolation,
durability}/` and are registered in `tests/CMakeLists.txt`. Each script must:

- Accept the minidb binary path as `$1`.
- Use `tests/lib/minidb_testlib.py` for the Python harness.
- Be deterministic given `--seed`.
- Print `PASS` on success, exit non-zero on failure.

### X2. Continuous fault injection

Add a `MINIDB_FAULT` env var that the build understands as a comma-separated
list of named injection points:

- `commit_before_fsync`
- `commit_after_fsync_before_state`
- `index_insert_skip`
- `heap_insert_skip`
- `ddl_mid_rewrite`
- `checkpoint_after_truncate`

`tests/acid/**` shell scripts toggle these to exercise the rare windows that
random kills cannot reliably hit.

### X3. Sanitizer matrix

Existing CI already runs ASAN / UBSAN / TSAN on a smoke subset. After ACID
tests land, extend the sanitizer matrix to include the full `tests/acid/`
suite (likely nightly only — TSAN is slow).

---

## Roll-up: minimum work to claim "ACID baseline"

| Property | P0 items | Tests required to pass | Status |
| --- | --- | --- | --- |
| Atomicity | A1, A2 | `acid/atomicity/*`, `recovery_smoke`, `crash_recovery_harness` | A1 done, A2 pending |
| Consistency | C1, C4 | `acid/consistency/*`, `consistency_test`, `index_unique_matrix` | C1 partial (NOT NULL+DEFAULT), C4 pending |
| Isolation | I1, I2 | `acid/isolation/*`, `mvcc_lock_regression` | I2 done; I1 pending (slot generation/wraparound); I3 doc-only done |
| Durability | D1, D2, D3, D4, D5 | `acid/durability/*`, `wal_buffer_pool_test`, `crash_recovery_harness` | D3 + D4 done; D1, D2, D5 pending |

Once every row's tests are green, the README can drop "experimental" from the
"ACID transactions" bullet and link this document as the contract.
