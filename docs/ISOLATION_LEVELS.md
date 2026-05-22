# MiniDB Isolation Levels

## Overview

MiniDB supports two isolation levels:

1. **Snapshot Isolation (SI)** -- the default level, suitable for most workloads.
2. **Serializable (SSI-lite)** -- an opt-in stricter level that adds read-write conflict detection on top of SI.

Set the isolation level for the current session with:

```sql
SET ISOLATION_LEVEL = SNAPSHOT;      -- default
SET ISOLATION_LEVEL = SERIALIZABLE;  -- opt-in
```

---

## Snapshot Isolation (Default)

Each transaction sees a **consistent snapshot** of the database taken at `BEGIN` time. All reads within that transaction observe the same point-in-time state, regardless of concurrent commits.

### Concurrency properties

- **Writers never block readers; readers never block writers.** Read-only queries always proceed without waiting.
- **Write-write conflicts** are resolved by the **first-writer-wins** rule. If two concurrent transactions modify the same row, the second writer receives an error at write time.

### Anomaly prevention

| Anomaly              | Prevented? |
|----------------------|------------|
| Dirty reads          | Yes        |
| Non-repeatable reads | Yes        |
| Lost updates         | Yes        |
| Write skew           | **No**     |
| Phantom reads        | **No**     |

### Implementation

Snapshot Isolation is implemented via MVCC with `xmin`/`xmax` tuple headers and a `TxnStatusLog` (the CLOG equivalent) for visibility decisions. See the [MVCC Implementation Details](#mvcc-implementation-details) section below.

---

## Serializable (SSI-lite)

The serializable level extends Snapshot Isolation with **read-write conflict detection**. Its goal is to catch the most common serialization anomalies -- in particular write skew -- that SI alone permits.

### How it works

1. Every row that a transaction reads (and that passes visibility checks) is recorded in the transaction's **read set**.
2. At commit time the read set is **intersected against the write sets** of all concurrently committed transactions.
3. If any overlap is found the committing transaction is **aborted** (first-committer-wins).

### What it catches

The classic **write-skew anomaly** is detected and prevented. For example, the doctors-on-call scenario -- two transactions each read that two doctors are on call, then each removes one -- will cause the second committer to abort.

### Known limitations relative to full SSI

MiniDB's serializable mode is intentionally labelled "SSI-lite" because it does **not** provide the same guarantees as full Serializable Snapshot Isolation (as implemented in PostgreSQL 9.1+). Specifically:

- **No predicate locking / range locks.** Some phantom-based anomalies that involve predicates over ranges of rows may not be detected.
- **Read-only transaction anomalies are not detected.** Full SSI can identify dangerous structures involving read-only transactions that observe an inconsistent ordering; MiniDB does not track these.
- **Detection is best-effort at the row level.** The intersection is performed on concrete row identifiers, not on the logical predicates that produced those rows.

In practice SSI-lite catches the most common real-world write-skew patterns while keeping the implementation simple and the overhead low.

---

## MVCC Implementation Details

### Tuple header

Every heap tuple carries two transaction identifiers:

| Field  | Meaning                                  |
|--------|------------------------------------------|
| `xmin` | Transaction that **created** the tuple.  |
| `xmax` | Transaction that **deleted** the tuple (invalid if the tuple is live). |

### Visibility rule

A tuple is **visible** to a reading transaction if:

1. `xmin` is **committed** (according to the TxnStatusLog), **and**
2. One of the following holds for `xmax`:
   - `xmax` is **invalid** (the tuple has not been deleted), or
   - `xmax` is **aborted**, or
   - `xmax` was **committed after** the reader's snapshot was taken.

### TxnStatusLog (CLOG)

For recycled transaction slots, visibility cannot rely solely on in-memory state. The `TxnStatusLog` persists the committed/aborted status of each transaction so that visibility checks remain correct even after the originating transaction's metadata has been reclaimed.

### HOT (Heap-Only Tuples)

When an `UPDATE` does not modify any indexed column, MiniDB can perform a **heap-only tuple** update. The new tuple version is placed on the same heap page and linked from the old version, avoiding index maintenance entirely. This is a significant optimization for update-heavy workloads where indexed columns rarely change.

---

## Statement-Level Atomicity

Within an explicit transaction (`BEGIN` ... `COMMIT`), each statement has an **implicit savepoint**:

- If a statement fails (e.g., constraint violation, type error), **only that statement is rolled back**. The surrounding transaction remains active and subsequent statements can still succeed.
- This matches PostgreSQL's behavior and allows applications to handle per-statement errors without losing the entire transaction.

---

## DDL and Locking

### Table-level locks acquired by DDL

| Operation                                    | Lock Level       |
|----------------------------------------------|------------------|
| `DROP TABLE` / `DROP INDEX` / `ALTER TABLE ADD/DROP/RENAME COLUMN` | AccessExclusive  |
| `CREATE INDEX`                               | Exclusive        |

### Transactional DDL

DDL operations participate in the transaction. If a transaction that has performed DDL is aborted -- whether explicitly or due to a crash -- the DDL changes are **rolled back** during recovery. This means, for example, that a `DROP TABLE` inside an uncommitted transaction does not permanently remove the table.

---

## Known Limitations

1. **Command-ID visibility.** Within a single transaction, rows inserted by an earlier statement are not guaranteed to be visible to a later `SELECT` in the same transaction when executed as part of the same statement. PostgreSQL solves this with command IDs (`cmin`/`cmax`) embedded in each tuple; MiniDB does not implement command IDs.

2. **Phantom reads under SI.** Range scans may observe phantom rows -- rows inserted by concurrent transactions that committed after the reader's snapshot but before the scan completes. This is an inherent property of Snapshot Isolation without predicate locking.

3. **SSI-lite completeness.** The serializable mode does not guarantee full serializability in all edge cases. It catches the most common write-skew patterns but may miss exotic anomalies involving read-only transactions or predicate-based conflicts. Applications that require strict serializability should implement application-level checks or use advisory locking for the affected code paths.
