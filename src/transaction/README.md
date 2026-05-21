# transaction

MVCC transaction manager. Hands out transactions, tracks active/committed
state in a slot table, and answers the visibility predicate that
`SeqScan` and friends use when walking version chains.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `Transaction` | `transaction.h`, `transaction.cpp` | Per-session handle: `txn_id`, snapshot id, snapshot of active txns, undo records |
| `TransactionManager` | same | Slot table, `begin/commit/rollback`, MVCC visibility |
| `TxnSlot` | `transaction.h` | One row of the bounded slot table: `txn_id`, `snapshot_id`, `commit_id`, state |
| `UndoRecord` | `transaction.h` | Per-row reversal note added during INSERT/UPDATE/DELETE |

## Invariants

- **Snapshot isolation** — `snapshot_id` is captured at `begin()`; readers
  see only versions committed strictly before `snapshot_id` and not in
  the snapshot's active-id set.
- **Commit ordering (A1)** — `TransactionManager::commit()` writes the
  WAL commit record and waits for `flush_commit` to confirm durability
  **before** flipping the slot to `kCommitted`. On `log_commit == 0`
  it falls back to the rollback path. See `docs/ACID_TODO.md` A1/D3.
- **Lost-update prevention (I2)** — UPDATE/DELETE executors take row
  locks and re-read `xmax`; on conflict they raise
  `could not serialize access due to concurrent update`.
- **Slot recycling is safe** — visibility tests re-read xmin/xmax slot
  states under the latch; recycled slots fail the `txn_id` compare and
  are treated as "not found", which the predicate maps to invisible.

## Tests

- `tests/unit/transaction_slots_test.cpp` — slot capacity beyond the
  legacy 256 boundary.
- `tests/acid/atomicity/commit_durability.py` — A1/D3 regression.
- `tests/acid/isolation/lost_update.py` — I2 regression.
- `tests/acid/isolation/mvcc_lock_regression.py` — broad MVCC + lock coverage.
- `tests/acid/isolation/write_skew.py` — documents SI's write-skew anomaly.
