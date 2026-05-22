# Known Limitations

This list is intentionally explicit so production users know where MiniDB still differs from mature databases.

## Storage and WAL

- WAL record replay is bounded, but the WAL format still uses a compact single-file log in this codebase.
- Index WAL key payloads are dynamically allocated, but their recorded length is still limited by the on-disk `u16` key-size field.
- PageServer supports persisted metadata and version lookup, but a full Raft-grade metadata quorum is outside the current implementation.

## Transactions

- Transaction slots are configurable through `max_active_transactions`; the old fixed 256-slot implementation has been removed.
- Transactions are coordinated inside one compute process.
- Multi-writer distributed transaction coordination is not implemented.

## Execution

- Hash join, sort, aggregate, and distinct spill to temp files and clean them up on normal error/cancel paths.
- Spill temp placement is controlled by `temp_dir`.
- Work memory is per operator and intentionally conservative.

## Optimizer

- Statistics have freshness checks and typed NDV, but not full histograms/MCV/multi-column correlation.
- Outer join rewrites are conservative.
- Adaptive cost learning is planned but not yet enabled.

## Indexes

- Single-column B+ tree indexes are directly supported for numeric, boolean, TEXT, and VARCHAR types.
- Multi-column composite B+ tree indexes are fully supported using the unified binary-comparable `IndexKey` representation, enabling composite unique constraints, prefix scans, and range scans.
- B+ tree operations use coarse tree locking; this is correct but can limit write-heavy concurrency.


## DDL semantics

- DDL is **fully transactional**. `CREATE TABLE`, `DROP TABLE`,
  `CREATE INDEX`, `DROP INDEX`, `ALTER TABLE ADD COLUMN`,
  `ALTER TABLE DROP COLUMN`, and `ALTER TABLE RENAME COLUMN` inside a
  `BEGIN..ROLLBACK` block are fully undone: catalog entries are
  restored, physical files are cleaned up (or restored from deferred
  deletion).
- `ALTER TABLE DROP COLUMN` uses PostgreSQL-style **metadata-only
  deletion**: the column is marked as logically dropped (`is_dropped`
  flag) without rewriting heap data. Physical tuple slots are preserved;
  new rows store NULL in dropped slots. This makes the operation O(1)
  regardless of table size and fully transactional.
- Crash between DDL execution and COMMIT/ROLLBACK may leave the catalog
  in the post-DDL state even if the transaction was never committed.
  This is a documented limitation for single-node educational use.
- Column-level `CHECK` predicates persist across restart and are
  evaluated on every INSERT / UPDATE.

## Query Processing

- No visibility map — `IndexOnlyScan` always performs a heap recheck.
- No query plan caching or parameterized plans — every execution re-plans from scratch.
- No `SELECT FOR UPDATE` or row-level locking beyond MVCC.
- `UNION` type alignment is implicit — no explicit type coercion.
- `CAST` failure behaviour is undefined for some type combinations.
- No `NULLS FIRST` / `NULLS LAST` syntax in `ORDER BY`.
- `NOT IN` with NULL on the right side correctly produces UNKNOWN per SQL standard.

## Recovery

- Index recovery uses lazy rebuild strategy — indexes are rebuilt from heap data after WAL replay, not physically redone from WAL records.
- DDL WAL records are audit-only markers — recovery does not act on them; crash between DDL and COMMIT may leave the catalog in the post-DDL state.
- No ARIES-style analysis/redo/undo phases — WAL replay is single-pass forward.
- No checkpoint dirty-page table or active-transaction table.
