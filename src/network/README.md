# network

TCP SQL server. Spawns worker threads, parses + executes SQL per
connection, enforces output buffer limits, idle timeouts, and server
cursor commands.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `Server` | `server.h`, `server.cpp` | TCP listen loop, connection state, statement dispatch |

## Connection lifecycle

1. Accept connection → worker thread picks it up.
2. Read a SQL statement, parse, plan, execute.
3. Implicit transaction: `txn_manager.begin()` runs before the first
   DML / SELECT statement when no explicit txn is active. Rollback runs
   automatically on executor error.
4. Idle connections beyond `connection_idle_timeout_ms` are reaped.

## Notable behaviour

- **Output buffer limit** — `client_output_buffer_limit_bytes` caps the
  send buffer to avoid one slow consumer holding the worker thread.
- **Prepared statements** — session-level only (no cross-connection
  cache). Cleared on DDL via `clear_prepared_cache()`.
- **Cursor commands** — `DECLARE CURSOR` / `FETCH` / `CLOSE` for paging
  large result sets without holding everything in memory.

## Tests

- `tests/concurrency/concurrent_test.py` — multi-client smoke.
- `tests/acid/atomicity/commit_durability.py`, `tests/acid/isolation/lost_update.py`,
  `tests/acid/isolation/write_skew.py`, `tests/acid/isolation/mvcc_lock_regression.py`
  — all drive the server over TCP via `TcpMiniDB` in `tests/lib/`.
