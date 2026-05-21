# Concurrency Control

MiniDB combines local MVCC snapshots, lock-manager coordination for writes, and bounded admission control.

## Transaction Slots

Active transactions are stored in a dynamically sized slot array controlled by `max_active_transactions`. The same setting is used by `ResourceManager` admission, so slot capacity and admission capacity stay aligned. A value of `0` disables the admission cap but the transaction manager still creates at least one slot.

Snapshots capture the active transaction ids at `BEGIN`. Visibility follows:

- A version is visible if its creator committed before the reader snapshot and was not active in the reader snapshot.
- A deleted version remains visible when the deleting transaction is uncommitted, aborted, or committed after the reader snapshot.
- Commit ids are assigned monotonically by the transaction manager.

## Locks and Resources

The lock manager owns tuple/table waits and deadlock handling. `ResourceManager` separately limits connections, queries, writes, memory reservations, temp bytes, and active transactions.

## Buffer Pool Concurrency

The buffer pool is partitioned by page id. Each partition has its own page table and LRU lock. Pin counts use standard C++ atomics. Cache misses are double-checked under the partition write lock so another thread cannot install the same page between read and write phases without being observed.

Dirty page flushes obey WAL-first ordering before the page store write.

## Isolation Level Guarantees

MiniDB offers **snapshot isolation (SI)**. It is not SERIALIZABLE.

- Each transaction reads from a snapshot taken at `BEGIN`. Reads see a
  consistent view as of that snapshot for the entire transaction.
- Two transactions writing the same row are detected: the second writer
  receives `Error: could not serialize access due to concurrent update`.
  This prevents the lost-update anomaly.
- Two transactions writing **different** rows whose values depend on the
  same predicate (the classic write-skew anomaly) are not detected and
  both will commit. Snapshot isolation permits this; a serializable
  guarantee would not.

`tests/acid/isolation/write_skew.py` documents the SI anomaly as
intentional behaviour; flipping it requires SSI or predicate locks and
is tracked in `docs/ACID_TODO.md`.

MiniDB does not currently parse `SET TRANSACTION ISOLATION LEVEL`, so
clients cannot accidentally request SERIALIZABLE and silently receive
SI — the statement is rejected by the parser.

## Current Boundaries

- Transaction management is local to one compute process.
- Distributed writes require an external timestamp/transaction service or single-writer ownership model.
- Long-running snapshots can delay version pruning until their snapshot is gone.
