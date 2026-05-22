# MiniDB ACID TODO (v2)

This is the working checklist for delivering **strict ACID semantics**.
Format: `[x]` = done with a guarding test, `[~]` = partial (caveats noted
inline), `[ ]` = pending.

Use [`docs/KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md) for the
user-facing summary; this file is the engineering plan.

---

## P0 — ACID boundary & acceptance criteria

```text
[ ] docs/ACID_MATRIX.md — single page, what we promise per mode
[ ] docs/ISOLATION_LEVELS.md — SI semantics, write-skew, phantoms
[ ] Single-node mode: ACID guarantees
[ ] Shared-storage mode: subset that is honoured
[~] Isolation level: Snapshot Isolation only (no Read Committed, no Serializable)
       — covered by tests/acid/isolation/write_skew.py
[x] README states "Snapshot isolation, not Serializable"
       — docs/CONCURRENCY_CONTROL.md
[x] Parser rejects SET TRANSACTION ISOLATION LEVEL (never emitted)
[ ] README states "no full predicate/range lock"
[~] IndexOnlyScan = heap-recheck fallback (no VM)
       — covered by tests/sql/sql_correctness_matrix.py
[~] Index recovery = lazy rebuild (no physical redo)
       — covered by tests/acid/durability/recovery_smoke.sh
[ ] DDL transactionality: explicitly documented
[~] ALTER TABLE crash-safety: limited (see LSN watermark fix, DROP COLUMN works after restart)
[~] UNIQUE/PK semantics under concurrency + crash: documented per case
[ ] HOT semantics: documented as "HOT-style same-page chain, indexes
    still maintained on indexed-column update"
[ ] Every ACID claim links to a test file in this doc
```

---

## A — Atomicity

### A1. Transaction state machine

```text
[~] States ACTIVE / COMMITTED / ABORTED defined (TxnState enum)
[ ] State COMMITTING — currently elided (commit_record fsync IS the transition)
[ ] State ABORTING — currently elided (rollback path runs synchronously)
[ ] State UNKNOWN_COMMIT_RESULT — currently elided
[x] All state transitions go through TransactionManager
       — src/transaction/transaction.cpp
[x] storage/executor/index do not flip final state
[x] COMMIT idempotent (no-op if already committed/aborted)
       — tests/acid/atomicity/commit_durability.py
[x] ROLLBACK idempotent
[x] commit record durable BEFORE slot flips to kCommitted (A1)
       — tests/acid/atomicity/commit_durability.py
[ ] ABORTING state persists across crash (currently in-memory undo only)
[ ] CLOG-equivalent transaction-status log
       — needed for "did txn N commit?" lookup after recovery
[~] Transaction id watermark persisted (control file `next_txn_id`)
[x] LSN watermark persisted across restart (control file `checkpoint_lsn`)
       — tests/regression/lsn_watermark_restart.py
```

### A2. Statement-level atomicity

```text
[~] Multi-row INSERT mid-failure: autocommit rolls back via undo,
    explicit txn leaves partial rows
       — tests/acid/atomicity/index_heap_atomic.py
[ ] Explicit savepoint per statement so explicit txns also get
    statement-level all-or-nothing
[~] UPDATE mid-failure: same as INSERT
[~] DELETE mid-failure: same as INSERT
[ ] CREATE INDEX failure must not leave a partial index visible
[ ] ALTER TABLE failure must not leave a partial catalog change
       — partly addressed by LSN watermark fix, still no WAL for DDL
[ ] OOM in executor must trigger statement rollback
[ ] I/O error in executor must surface (currently many silent failures)
[ ] temp-file error in sort/hash spill must trigger rollback
```

### A3. INSERT atomicity

```text
[x] heap tuple + every index entry atomic (A2)
       — tests/acid/atomicity/index_heap_atomic.py
[x] unique conflict leaves no visible row, no dangling index
       — tests/index/index_unique_matrix.sh
[x] crash before commit → row invisible
       — tests/acid/durability/crash_recovery_harness.py
[x] crash after commit → row visible
       — tests/acid/durability/recovery_smoke.sh
[~] crash during index maintenance → recovery lazily rebuilds index
[~] INSERT WAL redo idempotent (page_lsn skip)
[~] INSERT undo idempotent
[x] composite index INSERT atomic
       — tests/index/persistence_and_composite.sh
[x] unique index INSERT atomic
       — tests/index/index_unique_matrix.sh
```

### A4. DELETE atomicity

```text
[x] DELETE only marks xmax; old snapshots still see the row
       — tests/acid/isolation/mvcc_lock_regression.py
[x] DELETE rollback restores visibility
[~] Index entry lifecycle after DELETE: kept until GC sweep
[ ] Explicit test: long snapshot + DELETE + GC must not vacuum
    versions still visible to the snapshot
[~] DELETE WAL redo / undo idempotent
[x] DELETE crash before commit → row still visible
[x] DELETE crash after commit → row gone
```

### A5. UPDATE atomicity

```text
[x] Halloween-safe (always materialise targets)
       — tests/regression/update_halloween.py
[x] HOT update keeps old + new versions atomically
       — tests/ddl/hot_index_semantics.sh
[~] Non-indexed update goes HOT, no new index entry
[~] Indexed-column update creates new index entry,
    old entry stays until GC
[x] UPDATE rollback restores old version
[x] UPDATE crash before commit → old version visible
[x] UPDATE crash after commit → new version visible
[ ] UPDATE of UNIQUE key conflict checks + reservation release on rollback
       — partial today; explicit test pending
```

### A6. Undo / rollback

```text
[x] Undo records cover heap insert / delete / hot insert / hot delete
       — src/transaction/transaction.h (UndoType enum)
[ ] Undo records for index insert / index delete
       (currently rolled back via delete_index_entries from heap-undo,
        not as primary undo records)
[ ] Undo for unique reservation
[ ] Undo for page allocation
[ ] Undo for catalog create / drop / alter
[ ] Mid-rollback crash continuation (currently restart redoes the abort,
    which is idempotent — formalise + test)
[x] rollback releases row + table + key locks (LockManager::unlock_all)
[x] rollback releases unique reservation in autocommit
[ ] rollback closes cursors / executor state explicitly
```

---

## B — Consistency

### B1. PRIMARY KEY

```text
[x] PK auto-creates a unique index
[x] PK column implies NOT NULL
       — tests/acid/consistency/constraints_not_null_default.py
[ ] INSERT NULL into PK column rejected (test missing — verify path)
[ ] UPDATE PK to NULL rejected
[x] INSERT duplicate PK rejected
       — tests/index/index_unique_matrix.sh
[x] UPDATE to duplicate PK rejected
[x] crash after PK insert preserves uniqueness
[x] composite PK supported
[ ] composite PK NULL semantics explicitly documented
[x] concurrent INSERT same PK — exactly one wins
       — tests/acid/isolation/lost_update.py
[x] PK works with HOT update (id is indexed → non-HOT path)
```

### B2. UNIQUE

```text
[ ] UNIQUE NULL semantics documented (multiple NULLs allowed? standard says yes)
[x] single-column UNIQUE
[x] composite UNIQUE
       — tests/index/index_unique_matrix.sh
[x] INSERT unique check under correct snapshot
[x] UPDATE unique check under correct snapshot
[x] concurrent INSERT same key — exactly one wins
[~] INSERT rollback releases reservation (works in autocommit)
[~] UPDATE rollback releases reservation
[x] crash preserves uniqueness
[~] unique index rebuild detects conflicts
[ ] remote PageServer mode UNIQUE semantics documented
```

### B3. NOT NULL / DEFAULT / VARCHAR

```text
[x] NOT NULL enforced on INSERT
       — tests/acid/consistency/constraints_not_null_default.py
[x] NOT NULL enforced on UPDATE
[x] DEFAULT substituted when column omitted from INSERT
[x] DEFAULT + NOT NULL combination
[ ] DEFAULT expression typing/validation (only literals today)
[ ] VARCHAR(n) length limit on INSERT
[ ] VARCHAR(n) length limit on UPDATE
[ ] VARCHAR(n) length on CAST
[ ] VARCHAR(n) overflow behaviour documented (currently silent)
[ ] TEXT vs VARCHAR comparison semantics documented
```

### B4. Catalog consistency

```text
[ ] Catalog mutations transaction-scoped (currently apply immediately)
[ ] Catalog mutations WAL-logged
[ ] Catalog cache has schema version
[ ] Prepared statement invalidation on schema change
       (partially via clear_prepared_cache, but no version check)
[~] Table metadata / heap file consistency: maintained by save_catalog
[~] Index metadata / index file consistency
[ ] DROP TABLE / DROP INDEX physical file cleanup on rollback
       (currently irreversible)
[~] RENAME COLUMN updates dependencies (catalog only; expressions reparsed lazily)
[~] Recovery preserves catalog/file consistency (only because catalog is
     written outside the buffer pool with fsync)
```

### B5. Heap/index consistency

```text
[x] check_table_index_consistency() exists
       — src/database/database.cpp:746
[x] Optional startup check (consistency_check_on_startup = on)
       — tests/acid/consistency/post_recovery_verify.py
[ ] Index state machine: invalid / rebuilding / valid
[ ] Optimizer must refuse invalid indexes
[ ] Atomic flip to valid after rebuild
[~] Lazy rebuild on recovery (only when needs_index_rebuild set during WAL replay)
```

### B6. SQL standard semantics

```text
[~] NULL three-valued logic — partial; expression_evaluator currently has
     deliberate deviation (== NULL → false), documented inline
[~] IS NULL / IS NOT NULL
[~] AND / OR / NOT three-valued
[~] IN + NULL
[ ] NOT IN + NULL (correctness gap exists — see differential_sqlite output)
[~] BETWEEN + NULL
[~] LIKE + NULL
[~] LEFT JOIN ON vs WHERE semantics
[~] Predicate pushdown safety on outer joins
[~] GROUP BY / DISTINCT / ORDER BY NULL handling
[ ] NULLS FIRST / NULLS LAST syntax + behaviour
[~] COUNT(*) / COUNT(col) / SUM / AVG / MIN / MAX edge cases
[ ] UNION duplicate elimination + NULL
[ ] CAST failure behaviour
[~] CASE / COALESCE / NULLIF
```

---

## C — Isolation

### C1. Isolation level definition

```text
[ ] docs/ISOLATION_LEVELS.md
[x] Snapshot Isolation supported
       — tests/acid/isolation/mvcc_lock_regression.py
[ ] Read Committed: declared unsupported OR implemented
[ ] Serializable: declared unsupported (currently rejected by parser silence)
[x] write-skew documented as intentional SI anomaly
       — tests/acid/isolation/write_skew.py
[~] autocommit snapshot semantics (one snapshot per statement)
[~] explicit transaction snapshot pinned at BEGIN
[ ] read-only transaction snapshot (no SET TRANSACTION READ ONLY today)
[~] read-only compute storage_read_lsn semantics
       — docs/WAL_RECOVERY_PROTOCOL.md
[ ] Phantom behaviour explicitly tested + documented
```

### C2. MVCC visibility matrix

```text
[~] xmin committed + xmax variants — partial coverage via mvcc_lock_regression
[ ] explicit visibility matrix test covering all 8+ combinations
[~] read-own-write / read-own-delete
[~] update chain traversal
[~] HOT chain traversal
[x] old snapshot sees old version, new sees new
       — tests/acid/isolation/mvcc_lock_regression.py
[ ] command id (intra-statement visibility) — not implemented
[~] index scan visibility == seq scan visibility (heap recheck)
```

### C3. Write-write conflict

```text
[x] concurrent UPDATE same row
       — tests/acid/isolation/lost_update.py
[x] concurrent DELETE same row (same path as UPDATE)
[x] concurrent INSERT same unique key
[x] lost-update prevention
[x] write skew permitted (SI), documented
[ ] SELECT FOR UPDATE: declare unsupported OR implement
[~] Lock-wait timeout (controlled via config)
[x] Deadlock detection (wait-for graph DFS)
       — src/concurrency/lock_manager.cpp
[ ] Deadlock victim fairness: currently youngest-aborts, should be oldest-wins
[x] Aborted writer releases locks
[x] Committed writer releases locks
```

### C4. Lock manager

```text
[~] Lock compatibility matrix exists (4 modes)
[ ] Documented as a published matrix (not in CONCURRENCY_CONTROL yet)
[x] Table / record / key lock modes
[~] Lock upgrade (in-place upgrade exists, no downgrade)
[ ] Lock downgrade explicitly documented as unsupported
[ ] Wait-queue fairness policy
[~] Deadlock wait-for graph
[ ] Deadlock detection interval / threshold tunable
[ ] DDL ⇔ DML conflict matrix documented
[ ] CREATE INDEX ⇔ DML concurrency rule
[ ] DROP INDEX ⇔ live query rule
[ ] "key lock ≠ predicate lock" called out in docs
```

### C5. Serializable / predicate lock — branch "NOT SUPPORTED"

```text
[x] README/CONCURRENCY_CONTROL state SI only, not Serializable
[ ] docs/ISOLATION_LEVELS.md mirrors that
[~] Parser silently ignores SET TRANSACTION; should reject loudly
       — issue: gives the user no signal
[x] write-skew test recording current behaviour
[ ] phantom test recording current behaviour
[x] Key locks are not described as predicate locks
```

---

## D — Durability

### D1. WAL record format

```text
[~] WAL record header has lsn / txn_id / type / data_len
       — src/recovery/wal.h
[ ] Per-record magic byte
[ ] Per-record format version
[ ] Per-record CRC32
[ ] WAL partial-record detection on replay
[ ] Segment header with magic + version
[ ] Segment end marker (recovery currently relies on file length)
[x] WAL durable_lsn queryable
[~] corrupted record rejection (just stops replay)
```

### D2. WAL-before-data

```text
[x] page_lsn in page header
[x] page flush flushes WAL up to page_lsn
       — tests/unit/wal_buffer_pool_test.cpp
[x] BufferPool flush_page enforces WAL-first
[x] Checkpoint flush enforces WAL-first
       — tests/acid/durability/checkpoint_barrier.py
[x] LSN monotonic across restart
       — tests/regression/lsn_watermark_restart.py
[ ] PageStore write failure propagated everywhere (some void returns remain)
```

### D3. Heap redo

```text
[x] heap insert redo
[x] heap delete redo
[x] heap update redo
[~] HOT update redo (covered by update redo)
[x] heap page allocation handled at recovery
[~] heap page init redo (implicit via insert)
[ ] heap prune redo (no WAL record; prune is opportunistic)
[x] Redo idempotent via page_lsn skip
[x] crash during heap insert / delete / update covered
       — tests/acid/durability/crash_recovery_harness.py
```

### D4. Index recovery — current choice: REBUILD-BASED

```text
[~] Index marked needs_index_rebuild when WAL replay touches a heap row
[ ] Per-index state machine: valid / invalid / rebuilding
[ ] Optimizer refuses invalid index
[~] Recovery triggers rebuild_all_indexes()
[ ] Background rebuild option (currently synchronous)
[~] Unique index rebuild detects conflicts (relies on tree->insert returning false)
[ ] Rebuild failure leaves index marked invalid
[~] README states "lazy rebuild on recovery"
       — docs/KNOWN_LIMITATIONS.md
```

(Phase-2 alternative: physical btree redo. Tracked separately; not started.)

### D5. Checkpoint

```text
[ ] kCheckpointBegin record
[~] Single kCheckpoint record exists today
[ ] Checkpoint redo_lsn computed and persisted
[ ] Checkpoint includes dirty-page table
[ ] Checkpoint includes active-transaction table
[ ] Checkpoint includes transaction-status durable point
[x] Checkpoint barrier holds WAL latch through page flush (D2)
[x] crash before / during / after checkpoint covered
       — tests/acid/durability/checkpoint_barrier.py
[ ] Fuzzy checkpoint (currently stop-the-world via barrier)
```

### D6. Double-write / checksum / torn page

```text
[x] Page checksum covers body
[ ] Checksum also covers page_id / LSN (currently body-only)
[x] Doublewrite buffer with magic + checksum
[x] Doublewrite fsync before main write
[x] Crash + torn-page restored from doublewrite
       — tests/acid/durability/torn_page.py
[~] LSN regression detection (set_page_lsn clamps to forward-only)
```

### D7. DDL durability

```text
[ ] CREATE TABLE WAL record
[ ] DROP TABLE WAL record
[ ] CREATE INDEX WAL record
[ ] DROP INDEX WAL record
[ ] ALTER TABLE ADD/DROP/RENAME WAL records
[ ] Catalog change WAL
[ ] Physical-file create/delete WAL
[~] crash during ALTER TABLE handled (LSN watermark fix; rewrite was already in-place)
       — tests/regression/lsn_watermark_restart.py
[ ] crash during CREATE INDEX
[ ] crash during DROP TABLE: physical file orphan handling
```

### D8. I/O error durability

```text
[x] PageStore APIs return Result<void> / per-page status
       — src/storage/page_store.h
[~] Batch IO returns per-page status (Remote path)
[ ] DiskManager surfaces fsync errors all the way to commit
[ ] ENOSPC / EIO / partial-write tests
[ ] Permission-denied test
[~] Corrupted page detection (checksum)
[ ] Commit returns failure on WAL fsync error
       — current: log_commit returns 0 → commit() runs rollback path; good,
         but no test asserts the user sees a hard error
```

---

## E — HOT / GC / VACUUM

### E1. HOT

```text
[x] HOT update predicate: only non-indexed columns
[x] HOT update same-page only; falls back to non-HOT otherwise
[~] HOT root item + index entry consistency
[~] LP_REDIRECT chain traversal
[x] Index scan follows HOT chain
[x] Seq scan follows HOT chain
[~] Covering-index scan heap recheck follows HOT chain
[x] HOT rollback restores chain
[x] HOT crash recovery restores chain
[ ] HOT chain length bound + opportunistic prune metric
[~] HOT × unique index
[ ] HOT × vacuum interaction (vacuum not implemented as a dedicated op yet)
```

### E2. GC / VACUUM

```text
[x] global oldest-active-xid query
[x] DEAD tuple detection
[x] aborted tuple cleanup (via undo path)
[x] committed-deleted tuple cleanup
[~] HOT chain pruning
[~] LP_DEAD → LP_UNUSED transition
[ ] Free-space-map (not implemented)
[ ] Visibility-map (not implemented)
[~] Index cleanup during GC (lazy)
[ ] Long-transaction blocking-cleanup test
[~] Vacuum-vs-old-snapshot correctness
[~] Vacuum × concurrent scan
[ ] Freeze / all-frozen — not supported, document
```

### E3. Visibility Map / IndexOnlyScan

```text
[ ] visibility-map file/fork — NOT IMPLEMENTED
[ ] all-visible bit per page
[ ] all-frozen bit (declare unsupported)
[ ] heap insert/update/delete clears all-visible
[ ] vacuum sets all-visible
[~] IndexOnlyScan currently does heap recheck unconditionally
[~] Recheck fallback safe (visibility-correct)
```

---

## F — DDL ACID

### F1. CREATE / DROP

```text
[ ] CREATE TABLE transactional (rollback removes catalog entry + file)
[ ] DROP TABLE transactional (rollback restores file)
[ ] CREATE INDEX transactional
[ ] DROP INDEX transactional
[ ] Active scan isolation from DROP
[ ] Prepared statement invalidation
[~] crash recovery for CREATE TABLE (catalog persisted on fsync)
[ ] crash recovery for DROP TABLE (file cleanup)
```

### F2. ALTER TABLE

```text
[~] ADD COLUMN metadata only; existing tuples padded at read time
[x] ADD COLUMN DEFAULT applied to old rows on read
       — tests/acid/consistency/constraints_not_null_default.py
[ ] ADD COLUMN NOT NULL validation: rejects when table has rows w/o default
[ ] ADD COLUMN rollback
[~] ADD COLUMN crash recovery
[~] DROP COLUMN tuple layout rewrite in place
[x] DROP COLUMN crash-safe across restart
       — tests/regression/lsn_watermark_restart.py
[ ] DROP COLUMN rollback (currently irreversible)
[ ] RENAME COLUMN dependency update (expressions reparse on next plan)
[ ] RENAME COLUMN rollback
[ ] ALTER TABLE schema version bumped on success
[ ] ALTER TABLE invalidates prepared statements
[ ] ALTER TABLE invalidates catalog cache (currently no cache)
```

---

## G — Shared-storage / PageServer ACID

### G1. Documentation

```text
[~] README says experimental shared storage
[ ] docs/ACID_MATRIX.md: single-writer compute
[ ] read-only compute mode documented
[ ] No distributed transaction
[ ] No multi-writer
[ ] No Raft / quorum
[ ] Local replica dirs ≠ consensus replicas
[~] read_lsn / durable_lsn semantics
       — docs/WAL_RECOVERY_PROTOCOL.md
[~] future-page handling
[ ] Network-timeout-after-commit "unknown" behaviour
```

### G2. PageServer durability

```text
[x] metadata file checksum + end marker
       — src/storage/page_server.cpp
[x] WAL-image file checksum + magic + trailer
[ ] log_index file end-marker (rebuilt from WAL-image on startup)
[x] partial metadata write detection
[x] partial WAL-image write detection
[~] PageServer crash during write_page (batch IO returns per-page status)
[x] PageServer restart rebuilds versions/log_index
       — tests/unit/page_store_remote_test.cpp
[x] read_lsn unsatisfiable returns explicit error
[~] Batch read partial failure
[~] Batch write partial failure
```

### G3. Compute ⇔ PageServer consistency

```text
[~] commit_lsn ⇔ PageServer durable_lsn relationship
[~] Committed transaction pages durable or redoable
[~] read-only compute snapshot_lsn never reads future page
[ ] Stale-page detection
[ ] Duplicate write_page request idempotent
[ ] Retry write_page idempotent
[ ] Connection drop mid write
[ ] Timeout-after-commit "unknown" handling
[~] replica-dir write failure (best-effort)
[ ] PageServer consistency checker
```

### G4. Real distributed ACID — OUT OF SCOPE for now

```text
[ ] metadata service / consensus / failover / shard placement
[ ] 2PC / cross-shard atomic commit
[ ] distributed deadlock / timestamp oracle
```

Document explicitly as future / unsupported.

---

## H — Tests

```text
[x] tests/acid/atomicity/commit_durability.py
[x] tests/acid/atomicity/index_heap_atomic.py
[x] tests/acid/consistency/constraints_not_null_default.py
[x] tests/acid/consistency/post_recovery_verify.py
[x] tests/acid/isolation/mvcc_lock_regression.py
[x] tests/acid/isolation/lost_update.py
[x] tests/acid/isolation/write_skew.py
[x] tests/acid/durability/recovery_smoke.sh
[x] tests/acid/durability/wal_replay_slot_reuse.sh
[x] tests/acid/durability/crash_recovery_harness.py
[x] tests/acid/durability/torn_page.py
[x] tests/acid/durability/checkpoint_barrier.py
[x] tests/regression/update_halloween.py
[x] tests/regression/lsn_watermark_restart.py
[x] tests/sql/select_empty_list.py
[ ] tests/acid/atomicity/statement_savepoint.py — explicit txn statement rollback
[ ] tests/acid/atomicity/multi_index_atomic.py — multi-index INSERT atomicity (currently in index_heap_atomic, expand)
[ ] tests/acid/consistency/check_constraint.py — when CHECK is implemented
[ ] tests/acid/consistency/varchar_length.py
[ ] tests/acid/consistency/null_three_valued.py
[ ] tests/acid/consistency/notin_null.py
[ ] tests/acid/isolation/mvcc_visibility_matrix.py — exhaustive matrix
[ ] tests/acid/isolation/phantom.py
[ ] tests/acid/durability/wal_record_corruption.py
[ ] tests/acid/durability/io_error_propagation.py
[ ] tests/acid/durability/ddl_crash.py
[ ] tests/acid/durability/index_state_machine.py
[ ] tests/storage/pageserver_unknown_commit.py
```

---

## Recommended next 6 items — **ALL DONE**

1. **A1 — CLOG-equivalent.** ✅ `acid/atomicity/clog_status_log.py`.
   `src/transaction/txn_status_log.{h,cpp}` persists every commit/abort
   to `<db>/wal/txn_status.log`. is_visible() falls back to it when a
   slot has been recycled, closing the "recycled slot defaults to
   committed" hole for aborted xmins.

2. **D7 — DDL WAL records.** ✅ `acid/durability/ddl_wal.py`.
   Every CREATE / DROP / ALTER now emits a kDdl marker carrying op +
   table_id + aux + object name. Recovery ignores them today but the
   trail is durable for a future repair pass.

3. **B3 — VARCHAR(n) length.** ✅ `acid/consistency/varchar_length.py`.
   Column carries the bound, schema serialization round-trips it,
   Schema::validate_row enforces it on INSERT / UPDATE, both REPL and
   server CREATE-TABLE paths wire the parser's value through.

4. **D1 — WAL CRC32 + magic.** ✅ `acid/durability/wal_record_corruption.py`.
   Every WAL record gains a 32-bit magic prefix and a CRC32 over
   (header-with-crc-zeroed + payload). Recovery rejects torn / wild
   writes at the corrupted record instead of mis-reading garbage.

5. **B5/D4 — Index state machine + optimizer gate.** ✅
   `acid/durability/index_state_machine.py`. IndexEntry carries
   kValid / kInvalid / kRebuilding; WAL replay flips every index to
   kInvalid before rebuild; optimizer refuses non-kValid indexes;
   rebuild flips back atomically.

6. **C2 — Exhaustive MVCC visibility matrix.** ✅
   `acid/isolation/mvcc_visibility_matrix.py`. Persistent TCP sessions
   pin snapshots through nine xmin/xmax cases plus own-write /
   own-delete corners. SeqScan path verified; IndexScan visibility is
   tracked separately (see "IndexScan SI visibility" task — eager
   DELETE removes the index entry which can hide rows from old
   snapshots).

After those, the next tier is statement-level savepoints (A2),
visibility-map (E3), CHECK constraints (B3), and full predicate
locks / SSI (C5 — only if we commit to Serializable).

---

## Roll-up

| Property | P0 done | P0 remaining | Acceptance test bundle |
| --- | --- | --- | --- |
| Atomicity | A1 commit ordering + CLOG, A2 heap/index | statement savepoints, undo for indexes/DDL | `acid/atomicity/*`, `crash_recovery_harness` |
| Consistency | C1 NOT NULL+DEFAULT, B3 VARCHAR(n), C4 startup check, B5 index state | CHECK constraints, full NULL 3VL | `acid/consistency/*`, `index_unique_matrix` |
| Isolation | I2 lost-update, I3 SI doc, C2 MVCC matrix | docs/ISOLATION_LEVELS.md, IndexScan SI visibility, deadlock fairness | `acid/isolation/*`, `mvcc_lock_regression` |
| Durability | D1 WAL CRC, D2 barrier, D3 commit, D4 torn page, D7 DDL audit | checkpoint dirty-page table, fsync-error path | `acid/durability/*`, `wal_buffer_pool_test` |

Until the "P0 remaining" column is empty, MiniDB stays
**"snapshot-isolation, single-writer, lazy-index-rebuild ACID"** —
not full PostgreSQL-grade ACID, but a documented, testable subset.
