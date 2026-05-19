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

- Numeric and boolean single-column B+ tree indexes are directly supported.
- Composite or non-B+ tree-native keys may use encoded/hash keys depending on path.
- B+ tree operations use coarse tree locking; this is correct but can limit write-heavy concurrency.
