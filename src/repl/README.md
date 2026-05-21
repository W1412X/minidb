# repl

Interactive shell. Reads SQL from stdin, runs it against the embedded
`Database`, prints results. Also handles `--show-config` and a small
set of meta commands.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `Repl` | `repl.h`, `repl.cpp` | Read-eval-print loop, statement dispatch, implicit-transaction wrapping |

## Statement dispatch

The REPL forwards statements to the planner / executor pipeline,
mirroring `network/server.cpp` but for a single stdin session:

- DDL (`CREATE TABLE`, `ALTER TABLE`, ...) goes straight through to the
  `Database` API.
- DML / SELECT runs inside an implicit transaction when no explicit
  `BEGIN` is active.
- On executor error, the implicit transaction is rolled back via undo
  and the error message is printed.
- `EXPLAIN` and `EXPLAIN ANALYZE` short-circuit execution and emit the
  plan tree.

## Tests

Most shell-level tests drive the REPL by piping SQL into the `minidb`
binary, e.g. `tests/sql/sql_regression.sh`, `tests/sql/full_sql_suite.sh`,
`tests/acid/durability/recovery_smoke.sh`.
