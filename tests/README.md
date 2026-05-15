# MiniDB Test Scripts

## Core Regression

- `sql_regression.sh`: SQL planner/executor correctness, index paths, joins, MVCC basics.
- `sql_keyword_combinations.sh`: legal SQL keyword/table-alias combinations and reserved-word handling.
- `full_sql_suite.sh`: broad SQL syntax/functionality suite using `tests/test_suite.sql`.

## Performance And Resource Paths

- `performance_paths.sh`: DISTINCT, aggregate, union, join, and index scan performance paths.
- `performance_delivery.sh`: previous production-delivery checks for config, IndexOnlyScan, prepared plans, WAL/group-commit stats, and buffer-pool stats.
- `performance_optimizations.sh`: WAL page buffering, partitioned buffer-pool stats, heap Top-N sort, compiled filter predicates, late materialized SeqScan projection, and update-time version pruning smoke coverage.
- `join_optimizer.sh`: predicate/projection pushdown through joins, IndexLookupJoin selection, HashJoin small-side build, Grace hash spill, COUNT(*) join fast path, multi-join and SQL combination coverage.
- `resource_limits.sh`: admission, memory, temp-file, and result-limit behavior.
- `bulk_update_delete_perf.sh`: larger update/delete workload and index consistency.

## Storage, Recovery, And Concurrency

- `persistence_and_composite.sh`: persistence and composite index behavior.
- `recovery_smoke.sh`: WAL recovery and checkpoint smoke tests.
- `index_delete_rebuild.sh`: index delete/rebuild consistency.
- `concurrent_test.py` and `concurrent_test.sh`: multi-client concurrency coverage.

## Unit Tests

- `lock_manager_wait_test.cpp`: lock wait/timeout behavior.
- `tuple_value_edge_test.cpp`: tuple/value edge cases.
