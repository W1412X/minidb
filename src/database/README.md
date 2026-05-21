# database

`Database` is the top-level facade. It owns every subsystem singleton —
`DiskManager`, `BufferPool`, `Catalog`, `TransactionManager`,
`WalManager`, `LockManager`, `GarbageCollector`, heap files, index trees
— and exposes high-level operations the SQL layer calls into.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `Database` | `database.h`, `database.cpp` | Lifecycle, DDL, DML helpers, GC + checkpoint scheduling |

## Lifecycle

1. Constructor wires up `DiskManager` → `PageStore` → `BufferPool` →
   `WalManager` → `Catalog`, then runs `load_catalog()`.
2. `WalManager::recover()` replays + undoes the WAL. On success
   `rebuild_all_indexes()` and `flush()` follow.
3. If `consistency_check_on_startup = on` the constructor invokes
   `check_table_index_consistency()` and refuses to open the DB on
   disagreement (writes a descriptive line to stderr and
   `std::exit(1)`). See `docs/ACID_TODO.md` C4.
4. `start_background_maintenance()` spawns the GC + checkpoint thread.

## Important methods

| Method | What it does |
| --- | --- |
| `insert_index_entries(...)` | **Returns bool.** Every B+ tree entry must land for the call to succeed; on failure the caller's transaction undo cleans up the heap + partial indexes. |
| `delete_index_entries(...)` | Idempotent: tolerates missing entries (used by rollback). |
| `flush()` | Fsync WAL, then `pool->flush_all()`, then page-store fsync. |
| `checkpoint()` | Calls `WalManager::checkpoint(flush_pages_for_checkpoint_trampoline, this)` so the page flush runs inside the WAL-latch barrier (D2). |
| `check_table_index_consistency(err)` | Verifies every live heap tuple is reachable through each index. Used by C4. |

## Background maintenance

A single thread runs `background_maintenance_loop()`. It periodically:

- triggers a checkpoint when either `checkpoint_timeout` elapses or
  `wal.bytes_since_checkpoint` exceeds `checkpoint_wal_size`;
- runs `GarbageCollector::run_gc(...)`.

## Fault-injection

`MINIDB_FAULT` is a comma-separated env var honoured by `database.cpp`'s
`fault_active()`. Recognised points (test-only):

| Name | Effect |
| --- | --- |
| `index_insert_fail` | Force `insert_index_entries` to return false on the next entry. |
| `index_insert_silent` | Skip the tree insert AND report success (mimics the pre-A2 bug; used to seed inconsistent state for the C4 negative test). |

Production code never sets this variable; `fault_active()` is a single
`getenv` returning false in that case.

## Tests

Most ACID + integration tests exercise this module:
`tests/acid/**`, `tests/regression/**`, `tests/sql/**`, plus the C++
unit tests under `tests/unit/`.
