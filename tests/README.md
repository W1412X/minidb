# MiniDB Test Scripts

## Core Regression

- `sql_regression.sh`: SQL planner/executor correctness, index paths, joins, MVCC basics.
- `sql_keyword_combinations.sh`: legal SQL keyword/table-alias combinations and reserved-word handling.
- `full_sql_suite.sh`: broad SQL syntax/functionality suite using `tests/test_suite.sql`.
- `sql_correctness_matrix.py`: deterministic golden matrix for SQL semantics, joins, aggregation, subqueries, index-visible mutations, MVCC basics, and spill-sensitive operators. Supports `--seed` and `--stress`.
- `differential_sqlite.py`: runs supported SQL subsets against both MiniDB and SQLite, then compares normalized result sets. Supports `--seed`, `--cases`, and `--stress`.

## Performance And Resource Paths

- `performance_paths.sh`: DISTINCT, aggregate, union, join, and index scan performance paths.
- `performance_delivery.sh`: previous production-delivery checks for config, IndexOnlyScan, prepared plans, WAL/group-commit stats, and buffer-pool stats.
- `performance_optimizations.sh`: WAL page buffering, partitioned buffer-pool stats, heap Top-N sort, compiled filter predicates, late materialized SeqScan projection, and update-time version pruning smoke coverage.
- `join_optimizer.sh`: predicate/projection pushdown through joins, IndexLookupJoin selection, HashJoin small-side build, Grace hash spill, COUNT(*) join fast path, multi-join and SQL combination coverage.
- `resource_limits.sh`: admission, memory, temp-file, and result-limit behavior.
- `bulk_update_delete_perf.sh`: larger update/delete workload and index consistency.

## Storage, Recovery, And Concurrency

- `persistence_and_composite.sh`: persistence and composite index behavior.
- `remote_page_store.sh`: standalone `minidb_pageserver` integration, TCP RemotePageStoreClient, batch remote IO, restart recovery, remote metadata/WAL image persistence, remote-cost EXPLAIN, and remote client stats.
- `recovery_smoke.sh`: WAL recovery and checkpoint smoke tests.
- `crash_recovery_harness.py`: deterministic crash/restart harness that kills MiniDB around commit, rollback, dirty-page, and restart/checkpoint-like boundaries, then verifies logical state and repeated recovery idempotence.
- `index_delete_rebuild.sh`: index delete/rebuild consistency.
- `concurrent_test.py` and `concurrent_test.sh`: multi-client concurrency coverage.
- `mvcc_lock_regression.py`: deterministic server-mode concurrent transaction coverage. Supports `--seed` and `--stress`.

## Unit Tests

- `lock_manager_wait_test.cpp`: lock wait/timeout behavior.
- `tuple_value_edge_test.cpp`: tuple/value edge cases.
- `page_store_remote_test.cpp`: LocalPageStore compatibility, in-process PageServer semantics, durable-LSN rejection, batch IO, LogIndex restart rebuild, RO snapshot reads, future-page handling, TCP PageServer, and RemotePageStoreClient coverage.
- `wal_buffer_pool_test.cpp`: WAL-first dirty-page eviction ordering.
- `transaction_slots_test.cpp`: configurable transaction slot capacity above the old 256-slot boundary.

## One-command Entry Points

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
bash tests/run_all_tests.sh ./build/minidb --suite main --seed 12648430
```

Stress mode is intended for nightly/manual runs:

```bash
bash tests/run_all_tests.sh ./build/minidb --suite nightly --stress --seed 12648430
python3 tests/crash_recovery_harness.py ./build/minidb --stress --seed 12648500 --cases 5000
python3 tests/differential_sqlite.py ./build/minidb --stress --seed 12648600 --cases 10000
```

Every deterministic Python harness accepts `--seed`; failures include the seed in stderr.
