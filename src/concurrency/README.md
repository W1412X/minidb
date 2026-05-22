# concurrency

Lock manager and deadlock detector. Coordinates writers and DDL against
each other; readers go through MVCC and rarely touch this module.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `LockManager` | `lock_manager.h`, `lock_manager.cpp` | Table / record / key lock acquisition, release, and deadlock detection |
| `LockMode` | `lock_manager.h` | Access modes — AccessShare / RowExclusive / Exclusive / AccessExclusive |
| `LockObject` | `lock_manager.h` | Wait queue + holders for one lock target |
| `LockRequest` | `lock_manager.h` | One entry in a lock's wait queue |

## API surface

```cpp
Status lock_table(txn_id, table_id, mode);
Status lock_record(txn_id, table_id, rid, mode);
Status lock_key(txn_id, table_id, key, mode);
void   unlock_table/record/key(...);
void   unlock_all(txn_id);                // called by commit/rollback
```

`Status` is one of `kGranted`, `kWaiting`, `kDeadlock`, `kTimeout`,
`kError`. `Status::ok()` is true only for `kGranted`.

## Invariants

- **Lock ordering** — writers acquire WAL latch ahead of buffer-pool
  partition latches; the lock manager's own latch never overlaps with
  WAL-write latches.
- **Lock release on commit/rollback** — `TransactionManager` calls
  `unlock_all(txn_id)` from both commit and rollback paths.
- **Deadlock detection** — wait-for-graph DFS at lock acquisition time.
  Cycles are broken by aborting the youngest transaction (fairness
  improvements tracked in `docs/ACID_TODO.md` I4).
- **Write-write conflict surfacing (I2)** — the executors (not this
  module) re-read `xmax` after a row lock and raise
  `could not serialize access due to concurrent update` on mismatch.

## Notes

- MiniDB does **not** implement predicate / gap locks. Phantom
  prevention requires SSI; see `docs/CONCURRENCY_CONTROL.md`.
- Lock objects live in process-local memory only — distributed
  transactions are not supported.

## Tests

- `tests/unit/lock_manager_wait_test.cpp` — wait / timeout semantics.
- `tests/acid/isolation/mvcc_lock_regression.py` — concurrent SQL
  workload over the TCP server.
- `tests/acid/isolation/lost_update.py` — write-write conflict detection.
