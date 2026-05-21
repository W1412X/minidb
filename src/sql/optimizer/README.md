# sql/optimizer

Cost-based rewriter that runs on the logical `PlanNode` tree before
the executor factory builds the physical operators.

## What it does

- **Predicate pushdown** through inner joins, when a predicate refers
  to one side only.
- **Projection pushdown** for scan and aggregate paths, preserving any
  columns needed by ORDER BY / GROUP BY / HAVING / DISTINCT / UNION.
- **Scan-path selection** — IndexScan vs SeqScan vs IndexOnlyScan based
  on `ANALYZE` stats. IndexOnlyScan keeps an MVCC heap recheck because
  there is no visibility map yet.
- **Join-method selection** — HashJoin vs NestedLoopJoin vs IndexLookupJoin,
  including small-side build choice for HashJoin and Grace-hash spill
  when `work_mem` is low.
- **Aggregate fast path** — `COUNT(*)` over an inner join uses a
  dedicated `count_join` path when no other column is required.
- **Index-order scan** — ascending `ORDER BY` over an index-prefix
  skips the explicit sort.
- **Remote cost model** — `RemotePageStoreClient` paths use a higher
  random-IO cost; covered by `docs/OPTIMIZER_COST_MODEL.md`.

## What it does NOT do

- Full join enumeration (currently left-deep with greedy ordering).
- Histograms / MCV / multi-column statistics.
- Adaptive feedback.
- Correlated subquery decorrelation.

See `docs/OPTIMIZER_COST_MODEL.md` and the P3 section of
`docs/ACID_TODO.md` for the roadmap.

## Tests

- `tests/sql/join_optimizer.sh`
- `tests/performance/performance_paths.sh`
- `tests/performance/performance_optimizations.sh`
- `tests/sql/sql_correctness_matrix.py`
- `tests/sql/differential_sqlite.py`
