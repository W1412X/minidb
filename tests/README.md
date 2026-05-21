# MiniDB Tests

Tests are organised by what they exercise. Every test takes `./build/minidb`
as its first argument.

```
tests/
├── unit/          C++ unit tests built as standalone executables
├── sql/           SQL correctness, planner, optimizer, differential-vs-SQLite
├── ddl/           DDL / catalog evolution
├── index/         B+ tree and index maintenance
├── acid/          ACID-focused tests
│   ├── atomicity/    commit / rollback / crash atomicity (see docs/ACID_TODO.md)
│   ├── consistency/  constraint enforcement and post-recovery checks
│   ├── isolation/    MVCC / lock / concurrent transactions
│   └── durability/   WAL, checkpoint, crash recovery
├── concurrency/   multi-client TCP concurrency
├── storage/       page store, remote PageServer
├── performance/   performance smoke / regression
├── regression/    broad end-to-end regression and consistency
└── lib/           shared Python helpers (minidb_testlib)
```

## Running

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j4

# All registered CTest cases
ctest --test-dir build --output-on-failure

# Documented end-to-end suite
bash tests/run_all_tests.sh ./build/minidb --suite main --seed 12648430
```

`run_all_tests.sh` writes a markdown report (default `build/test-report.md`)
and one log file per test under `build/test-logs/`.

### Suites

| Suite      | Used in CI                                            |
| ---------- | ----------------------------------------------------- |
| `pr`       | Pull requests — fast subset of SQL + recovery + MVCC. |
| `main`     | Push to `main` and manual dispatch — everything.      |
| `nightly`  | Scheduled run with `--stress`.                        |

Stress mode is meant for nightly/manual runs:

```bash
bash tests/run_all_tests.sh ./build/minidb --suite nightly --stress --seed 12648430
python3 tests/acid/durability/crash_recovery_harness.py ./build/minidb --stress --seed 12648500 --cases 5000
python3 tests/sql/differential_sqlite.py ./build/minidb --stress --seed 12648600 --cases 10000
```

Every deterministic Python harness accepts `--seed`; failures print the seed
on stderr so you can reproduce locally.

## Catalogue

### `unit/` — C++ unit tests

- `lock_manager_wait_test.cpp`: lock wait / timeout behavior.
- `tuple_value_edge_test.cpp`: tuple/value edge cases.
- `page_store_remote_test.cpp`: `LocalPageStore` compatibility, in-process
  PageServer semantics, durable-LSN rejection, batch IO, LogIndex restart
  rebuild, RO snapshot reads, future-page handling, TCP PageServer, and
  `RemotePageStoreClient` coverage.
- `wal_buffer_pool_test.cpp`: WAL-first dirty-page eviction ordering.
- `transaction_slots_test.cpp`: configurable transaction-slot capacity above
  the legacy 256-slot boundary.
- `btree_property_test.cpp`: B+ tree property tests against `std::multimap`.
- `index_key_btree_test.cpp`: composite `IndexKey` encoding and lookup
  invariants.

### `sql/` — SQL correctness

- `sql_regression.sh`: planner/executor correctness, index paths, joins, MVCC
  basics.
- `sql_keyword_combinations.sh`: legal SQL keyword/table-alias combinations
  and reserved-word handling.
- `full_sql_suite.sh`: broad SQL syntax/functionality suite using
  `tests/sql/test_suite.sql`.
- `join_syntax_strict.sh`: strict join syntax acceptance.
- `join_optimizer.sh`: predicate/projection pushdown through joins,
  `IndexLookupJoin` selection, hash-join build side, Grace-hash spill,
  `COUNT(*)` join fast path, and multi-join coverage.
- `sql_correctness_matrix.py`: deterministic golden matrix for SQL semantics,
  joins, aggregation, subqueries, index-visible mutations, MVCC basics, and
  spill-sensitive operators.
- `differential_sqlite.py`: runs supported SQL subsets against MiniDB and
  SQLite and compares normalised result sets.
- `test_all.sql`, `test_features.sql`, `test_suite.sql`: shared SQL fixtures.

### `ddl/` — DDL / catalog evolution

- `alter_table_semantics.sh`: ADD/DROP/RENAME COLUMN behavior.
- `hot_index_semantics.sh`: HOT-style same-page version-chain semantics on
  indexed and non-indexed updates.

### `index/` — Index maintenance

- `persistence_and_composite.sh`: persistence and composite index behavior.
- `index_delete_rebuild.sh`: index delete/rebuild consistency.
- `index_unique_matrix.sh`: unique-index conflict matrix for single and
  composite keys.

### `acid/` — ACID-focused tests

See [docs/ACID_TODO.md](../docs/ACID_TODO.md) for the full hardening plan.

- `acid/isolation/mvcc_lock_regression.py`: deterministic server-mode
  concurrent transaction coverage.
- `acid/durability/recovery_smoke.sh`: WAL recovery and checkpoint smoke
  tests.
- `acid/durability/wal_replay_slot_reuse.sh`: WAL replay correctness when
  transaction slots are reused.
- `acid/durability/crash_recovery_harness.py`: deterministic crash/restart
  harness that kills MiniDB around commit, rollback, dirty-page, and
  checkpoint-like boundaries, then verifies logical state and idempotent
  recovery.
- `acid/atomicity/` and `acid/consistency/` are intentionally empty placeholders
  for the test items listed in `docs/ACID_TODO.md`. Add new tests as the
  corresponding hardening items land.

### `concurrency/` — Multi-client

- `concurrent_test.py`: multi-connection TCP smoke.

### `storage/` — Page store / PageServer

- `remote_page_store.sh`: standalone `minidb_pageserver` integration,
  RemotePageStoreClient, batch remote IO, restart recovery, remote
  metadata/WAL image persistence, remote-cost EXPLAIN, and remote client
  stats.

### `performance/` — Performance smoke

- `performance_paths.sh`, `performance_delivery.sh`,
  `performance_optimizations.sh`, `bulk_update_delete_perf.sh`.

### `regression/` — Broad regression

- `production_regression.sh`: production-style mixed workload.
- `resource_limits.sh`: admission, memory, temp-file, and result-limit
  behavior.
- `consistency_test.py`, `bug_verify.py`, `comprehensive_test.py`,
  `ultimate_test.py`.
