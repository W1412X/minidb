# README Capability Gap Checklist

This document reviews README claims against the current core implementation and
turns the gaps into an implementation checklist.

## Conclusion

The reported concerns are mostly reasonable. Several README statements describe
code paths that exist, but the implementation is not yet complete enough to call
the feature production-grade database semantics.

The main distinction used here:

- **Supported**: semantics are implemented end to end and covered by targeted
  tests.
- **Partial / experimental**: a code path exists, but important correctness,
  recovery, concurrency, or boundary semantics are missing.
- **README downgrade needed**: the current implementation should not be
  documented as fully supported until the listed work is complete.

## Evidence Summary

- `src/index/btree.h` now exposes `IndexKey` as the physical B+ tree key and
  stores fixed-size encoded composite key slots. `IndexKeySchema` carries type,
  direction, null ordering, and bytewise-collation metadata.
- Composite index paths now build ordered multi-column `IndexKey` values for
  lookup, prefix scan, range scan, insert, update, delete, and unique checks.
- `src/storage/page_store.h` exposes `void read_page`, `void write_page`,
  `void flush`, and `void delete_file`, so storage errors cannot be propagated
  precisely through the core `PageStore` interface.
- `IndexOnlyScanExecutor` now performs a heap visibility recheck fallback before
  returning covered key values. A visibility map remains a future performance
  optimization, not a correctness dependency.
- PageServer has persisted metadata and log-index reconstruction paths, sharded
  metadata locks, and checksum/trailer detection for metadata/WAL-image partial
  writes. It should still remain documented as experimental shared storage.
- The optimizer is rule-assisted and cost-based in selected paths, but it does
  not yet have full histogram/MCV/multi-column statistics, full join
  enumeration, or adaptive feedback.

## README Claims To Downgrade Now

- [x] `composite indexes`
  - Current state: ordered physical `IndexKey` support is implemented for
    equality, prefix/range scan, index maintenance, and composite unique checks.

- [x] `B+ tree indexes for single-column and composite keys`
  - Current state: B+ tree public APIs use `IndexKey`; composite and varchar/text
    keys are stored as encoded physical keys.

- [x] `IndexOnlyScan`
  - Current state: MVCC-safe covering-index scan with heap recheck fallback.
    Visibility-map based heapless reads remain future performance work.

- [x] `HOT-style same-page update chains`
  - Current state: line-pointer and version-chain infrastructure exists, but
    the update path still updates indexes in the HOT path.
  - README wording: `HOT-style same-page version-chain infrastructure; full
    PostgreSQL-style HOT semantics are under validation`.

- [x] `WAL records for index changes + crash recovery`
  - Current state: index WAL records exist, but recovery can still depend on
    lazy rebuild.
  - README wording: `WAL recovery with lazy index rebuild after recovery; full
    physical index redo is under development`.

- [x] `RemotePageStoreClient batch read/write, retry, timeout`
  - Current state: batch/RPC paths exist, but the `PageStore` interface cannot
    return per-page status.
  - README wording: `experimental remote page IO with batch RPC; per-page error
    propagation is under development`.

- [x] `Cost-based optimizer`
  - Current state: initial cost-based path selection.
  - README wording: `rule-assisted cost-based path selection; full CBO
    statistics and join-order optimization are under development`.

- [x] `ALTER TABLE ADD/DROP/RENAME COLUMN`
  - Current state: basic DDL path, not full layout migration / dependency /
    crash-safe DDL semantics.
  - README wording: `basic ALTER TABLE support; full layout migration and
    crash-safe dependency invalidation are under validation`.

## P0: Interface And Storage Correctness

- [x] Replace `PageStore` void methods with result-returning APIs:
  - [x] `Result<void> read_page(PageId, byte*)`
  - [x] `Result<void> write_page(PageId, const byte*, LSN)`
  - [x] `Result<void> flush()`
  - [x] `Result<void> delete_file(const String&)`
  - [x] Convert callers in `BufferPool`, recovery, `RemotePageStore`, and
    `RemotePageStoreClient`.
  - [x] Treat retryable remote failures, checksum mismatch, ENOSPC, fsync
    failure, and fatal corruption as separate error classes.

- [x] Return per-page status from batch page IO:
  - [x] `read_pages` reports individual read failures.
  - [x] `write_pages` reports individual write/fdatasync/checksum failures.
  - [x] Batch write must clearly define partial-success semantics.
  - [x] BufferPool and checkpoint code must stop treating failed writes as
    successful flushes.

- [x] Introduce real physical index keys:
  - [x] Add `IndexKey` and `IndexKeySchema`.
  - [x] Store column type, sort direction, null ordering, and collation id.
  - [x] Implement NULL-aware comparison.
  - [x] Implement stable fixed-slot encoding for fixed and variable types.
  - [x] Change B+ tree public APIs from `Value` to `IndexKey`.
  - [x] Keep compatibility for existing single-value indexes through
    `IndexKey::single`.

- [x] Implement true composite index semantics:
  - [x] Ordered multi-column key compare.
  - [x] Composite equality lookup.
  - [x] Composite prefix scan.
  - [x] Composite range scan when the leading prefix is constrained.
  - [x] Composite unique checking.
  - [x] Remove hash-only composite lookup from correctness-critical paths.

- [x] Define text/varchar index support explicitly:
  - [x] Implement varlen values inside fixed-size encoded `IndexKey` slots.
  - [x] Use bytewise text comparison via `String::compare`.
  - [x] Define collation behavior as bytewise collation id `0`.
  - [x] Define max key size behavior: reject keys whose encoded form exceeds
    the index slot size.
  - [x] Add string equality/range and overlong-key rejection tests.

- [x] Make `IndexOnlyScan` MVCC-correct:
  - [ ] Add a visibility map or equivalent all-visible proof.
  - [ ] Clear all-visible bits on heap insert/update/delete.
  - [ ] Set all-visible bits only after GC/vacuum proves no active snapshot can
    need old versions.
  - [x] Fall back to heap visibility check when visibility proof is absent.
  - [x] Keep the covering-index path only after heap visibility recheck.

- [x] Tighten B+ tree deletion APIs:
  - [x] Make `remove(key)` valid only for unique indexes or remove the method.
  - [x] Use `remove(key, rid)` for non-unique indexes.
  - [x] Add duplicate-key deletion tests.
  - [x] Add invariant checks after delete, borrow, merge, and root shrink.

- [x] Remove `collect_all_entries` / `rebuild_from_entries` from normal B+ tree
  mutation paths:
  - [x] Keep rebuild only for explicit recovery/maintenance paths.
  - [x] Ensure delete mutation is page-local; empty leaves are unlinked
    incrementally.
  - [x] Add property tests against `std::multimap`.

- [x] Document and enforce BufferPool frame state:
  - [x] States: empty/free, loading, resident clean/dirty, flushing, evicting.
  - [x] Protect `page_id`, dirty flag, IO-in-flight state, pin count, and LRU
    node consistently.
  - [x] Coalesce concurrent read misses for the same page.
  - [x] Ensure WAL-first durable LSN check remains mandatory before dirty flush.

- [x] Split PageServer global locking:
  - [x] Partition metadata/log-index/version locks by page shard.
  - [x] Avoid holding metadata locks while performing disk IO.
  - [x] Add read/write contention tests for hot page and many-page workloads.

- [x] Harden PageServer metadata and WAL-image files:
  - [x] Add checksum/trailer records for metadata and WAL images.
  - [x] Detect partial metadata writes.
  - [x] Detect partial WAL-image writes.
  - [x] Make restart reconstruction reject corrupt/truncated records instead of
    trusting partial state.
  - [x] Return explicit error when a requested `read_lsn` cannot be satisfied.

## P1: Correctness Hardening For Existing Features

- [ ] HOT update semantics:
  - [ ] Non-indexed-column update must not create new index entries.
  - [ ] Indexed-column update must create new index entries and invalidate old
    entries according to MVCC visibility.
  - [ ] Index scan must follow HOT chain to the visible version.
  - [ ] Rollback must restore the chain.
  - [ ] Crash recovery must leave the chain consistent.
  - [ ] Pruning must preserve versions visible to old snapshots.

- [ ] Index maintenance:
  - [ ] INSERT creates all index entries atomically with heap visibility.
  - [ ] DELETE removes or invalidates index entries consistently.
  - [ ] UPDATE changes indexed and non-indexed columns correctly.
  - [ ] Recovery verifies heap/index consistency.
  - [x] Add table/index consistency checker.

- [ ] UNIQUE / PRIMARY KEY semantics:
  - [ ] Define UNIQUE NULL behavior.
  - [ ] Concurrent insert of same key must conflict deterministically.
  - [ ] Insert rollback must release unique-key reservation.
  - [ ] Update unique key must check conflicts.
  - [ ] Composite unique indexes must use true composite keys.
  - [ ] Crash recovery must preserve uniqueness.

- [ ] ALTER TABLE semantics:
  - [ ] `ADD COLUMN DEFAULT` old-row read behavior.
  - [ ] `ADD COLUMN NOT NULL` validation/rejection rules.
  - [ ] `DROP COLUMN` tuple-layout compatibility.
  - [ ] `RENAME COLUMN` updates catalog/index/dependency metadata.
  - [ ] WAL records for DDL.
  - [ ] Crash recovery for DDL.
  - [ ] Catalog cache / prepared plan invalidation.
  - [ ] DDL lock protocol with concurrent DML/query.

- [ ] WAL and index recovery:
  - [ ] Decide whether index changes are physically redone or rebuilt.
  - [ ] If redone, make index WAL replay complete and idempotent.
  - [ ] If rebuilt, document lazy rebuild as a correctness dependency.
  - [ ] Test crash during index build, insert, update, delete, and checkpoint.

- [ ] MVCC visibility matrix:
  - [ ] Insert/update/delete commit and rollback.
  - [ ] Read-own-write and read-own-delete.
  - [ ] Long transaction plus GC/pruning.
  - [ ] Snapshot isolation guarantees and non-guarantees.
  - [ ] Old snapshot with index scan, covering-index scan, and seq scan.

- [ ] Locking semantics:
  - [ ] Document lock compatibility matrix.
  - [ ] Test lock upgrade and downgrade if supported.
  - [ ] Test rollback/commit release.
  - [ ] Test DDL lock conflicts with reads/writes/index creation.
  - [ ] Clarify that key locks are not full predicate/range locks unless those
    are implemented.

## P2: SQL Semantics

- [ ] NULL three-valued logic:
  - [ ] Comparisons with NULL.
  - [ ] `IS NULL` / `IS NOT NULL`.
  - [ ] `AND` / `OR` / `NOT` truth tables.

- [ ] `NOT IN` and NULL:
  - [ ] `NOT IN` with NULL in the right side.
  - [ ] `NOT IN` with NULL left expression.
  - [ ] Empty subquery behavior.

- [ ] LEFT JOIN semantics:
  - [ ] `LEFT JOIN ... ON` vs `WHERE` predicate placement.
  - [ ] Right-side predicates must not be pushed below the join when doing so
    changes null-extension semantics.
  - [ ] No-match rows must preserve left rows and fill right columns with NULL.

- [ ] GROUP/DISTINCT/ORDER NULL semantics:
  - [ ] `GROUP BY NULL`.
  - [ ] `DISTINCT NULL`.
  - [ ] `ORDER BY NULL` ordering policy.
  - [ ] Document whether NULLS FIRST/LAST syntax is supported.

- [ ] Aggregate semantics:
  - [ ] `COUNT(*)` vs `COUNT(col)`.
  - [ ] `SUM` / `AVG` on empty input.
  - [ ] `MIN` / `MAX` with all-NULL input.
  - [ ] Multi-column group keys.

- [ ] Type and constraint semantics:
  - [ ] CAST failure behavior.
  - [ ] `VARCHAR(n)` length enforcement.
  - [ ] `DEFAULT` and `NOT NULL`.
  - [ ] UNION type alignment if UNION remains documented.

## P3: Optimizer

- [ ] Improve cardinality/statistics:
  - [ ] Persist row count and page count.
  - [ ] Persist per-column null fraction, NDV, min/max.
  - [ ] Add histograms.
  - [ ] Add MCV lists.
  - [ ] Add multi-column statistics for common composite predicates.
  - [ ] Track statistics freshness by table version/row-count delta.

- [ ] Join selectivity:
  - [ ] Use clamped NDV estimates for equality joins.
  - [ ] Account for NULL fraction.
  - [ ] Avoid impossible cardinality estimates.
  - [ ] Validate estimated vs actual rows in `EXPLAIN ANALYZE`.

- [ ] Join ordering:
  - [ ] Implement left-deep dynamic programming for 3-8 inner joins.
  - [ ] Preserve outer-join semantic barriers.
  - [ ] Keep cross joins explicit and expensive.
  - [ ] Add regression tests for three- and four-table joins.

- [ ] Predicate pushdown safety:
  - [ ] Inner join pushdown only for predicates referencing one side.
  - [ ] LEFT JOIN `ON` and `WHERE` predicates handled separately.
  - [ ] HAVING predicates pushed only when semantically equivalent.
  - [ ] Subquery decorrelation rules isolated and tested.
  - [ ] `NOT IN` with NULL never rewritten unsafely.

- [ ] Projection pushdown safety:
  - [ ] Preserve columns needed by `ORDER BY`.
  - [ ] Preserve columns needed by `GROUP BY`.
  - [ ] Preserve columns needed by `HAVING`.
  - [ ] Preserve columns needed by `DISTINCT` / `UNION`.
  - [ ] Preserve correlated subquery columns.
  - [ ] Separate covering-index projection from MVCC visibility proof.

- [ ] Remote cost model:
  - [ ] Use measured remote read/write latency.
  - [ ] Model sequential remote scan vs random remote lookup separately.
  - [ ] Include batch size and page-cache hit rates.
  - [ ] Penalize index lookup joins when random remote IO dominates.

## P4: Executor And Spill Paths

- [ ] HashJoin:
  - [ ] Duplicate key correctness.
  - [ ] NULL join key correctness.
  - [ ] Build-side selection correctness.
  - [ ] Very small `work_mem`.
  - [ ] Partition skew.
  - [ ] Spill file cleanup on error/cancel.
  - [ ] Crash/startup cleanup of stale temp files.

- [ ] Aggregate:
  - [ ] NULL and empty-set semantics.
  - [ ] Multi-column group key hash/compare.
  - [ ] Text/varlen group keys.
  - [ ] AVG spill merge combines sum and count correctly.
  - [ ] Spill file lifecycle under cancel/error.

- [ ] Sort:
  - [ ] Multi-column `ORDER BY`.
  - [ ] ASC/DESC mixed ordering.
  - [ ] NULL ordering.
  - [ ] Text ordering.
  - [ ] `LIMIT 0`.
  - [ ] `LIMIT` + `OFFSET`.
  - [ ] Top-N result equals full sort then limit.
  - [ ] External sort result equals in-memory sort.

- [ ] Executor lifecycle:
  - [ ] `init` / `next` / `close` / reset rules.
  - [ ] `close` idempotence.
  - [ ] `next` after EOF.
  - [ ] Query cancel releases all resources.
  - [ ] Cursor close releases executor state.
  - [ ] `query_memory_limit` and `temp_file_limit` enforced consistently.

## P5: Test System

- [x] B+ tree property testing against `std::multimap`.
- [ ] Hash map property testing against `std::unordered_map`.
- [ ] SQL differential testing expanded to:
  - [ ] NULL logic.
  - [ ] JOIN variants.
  - [ ] GROUP BY / HAVING.
  - [ ] ORDER BY / LIMIT / OFFSET.
  - [ ] INSERT / UPDATE / DELETE.
- [ ] Crash harness expanded to:
  - [ ] DDL.
  - [ ] Index build.
  - [ ] Checkpoint.
  - [ ] PageServer metadata.
  - [ ] PageServer WAL-image partial write.
- [ ] Sanitizer CI:
  - [ ] ASAN.
  - [ ] UBSAN.
  - [ ] TSAN.
- [ ] Nightly stress:
  - [ ] Save seed on every failure.
  - [ ] Save SQL/history trace.
  - [ ] Save MiniDB log and reference DB output.
  - [ ] Save crash injection point.

## Recommended Execution Order

1. Downgrade README claims and link this checklist. (done)
2. Convert `PageStore` to result-returning APIs and propagate errors. (done)
3. Add table/index consistency checker and B+ tree property tests. (done)
4. Introduce `IndexKey` / `IndexKeySchema`. (done)
5. Rebuild composite and text/varchar index support on top of `IndexKey`. (done)
6. Make covering-index scan MVCC-correct or keep it documented as non-index-only. (done via heap recheck fallback)
7. Finish HOT semantics after index maintenance and visibility checks are stable.
8. Harden WAL/index recovery and DDL recovery.
9. Expand optimizer statistics, pushdown safety tests, and join ordering.
10. Expand spill executor boundary tests and sanitizer/nightly CI.

## Notes On Scope

- Prefix compression, TOAST-like overflow, and locale-aware collation are not
  mandatory for a first complete text index implementation. It is acceptable to
  start with bytewise collation, a documented max key size, and clear rejection
  of unsupported cases.
- A visibility map is not the only possible implementation of MVCC-safe
  index-only scans. However, without a visibility proof or heap recheck, the
  current path should not be described as full index-only scan.
- PageServer currently should be treated as experimental shared storage. Making
  it production-grade requires protocol-level error semantics, partitioned
  concurrency, crash-safe metadata, and replication/failover work beyond the
  current MVP.
