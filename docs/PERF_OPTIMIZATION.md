# MiniDB Performance Optimization Plan

This document tracks algorithmic optimization work on the execution layer,
storage layer, and optimizer. Each round is a self-contained commit with
tests. Caches (statement cache, plan cache, etc.) are out of scope — only
*algorithm/design-level* improvements that change asymptotic or constant
factors of the work itself.

## Baseline (post-prior-optimizations)

Measured on macOS / SSD / single-threaded against `tests/performance/bench.py`:

| Operation | MiniDB | SQLite | Gap |
|-----------|--------|--------|-----|
| bulk_insert (5000 rows, batch=40)   | 7,441 r/s   | 118,703 r/s | 16× |
| seq_scan  (COUNT*)                  | 332 q/s     | 123,750 q/s | 372× |
| pk_lookup (200 random)              | 1,427 q/s   | 103,317 q/s | 72× |
| range_scan (100-row ranges)         | 333 q/s     | 89,392 q/s  | 268× |
| update (autocommit)                 | 1,710 o/s   | 3,472 o/s   | 2× ⭐ |
| delete_insert                       | 3,890 o/s   | 6,847 o/s   | 1.8× ⭐ |
| join (self, 100×100)                | 131 q/s     | 10,876 q/s  | 83× |
| txn_batch (50-row txns)             | 3,090 r/s   | 109,795 r/s | 35× |

Most of the gap on small queries is *per-statement parse/plan* overhead
(SQLite has automatic statement caching; we don't and won't, per scope).
This plan targets the **per-row execution cost** and the **storage-layer
work-per-operation** in larger queries.

## Optimization Rounds

### R1 — Pre-allocated filter eval context (DONE)
- **Problem**: `FilterExecutor::eval_compiled` did `std::vector<Value> stack(N)` heap allocation per row.
- **Fix**: Allocate the stack once at `init()`, reuse across rows.
- **Affects**: Every WHERE clause.

### R2 — Filter pushdown into SeqScan / IndexScan (DONE)
- **Problem**: `Filter→SeqScan` materialized every row (heap, vector<Value>,
  visibility recheck) before evaluating the predicate. For selective WHEREs
  >95% of materialized work is thrown away.
- **Fix**: Plan node carries optional pushed-down predicate; scan evaluates
  it inline against the page-resident tuple header / partial tuple, and
  skips emit on `false`. Logically a `Filter(Scan(t))` becomes a single
  `Scan(t, pred)`.
- **Affects**: Every query with WHERE.

### R3 — Projection-aware tuple decode (DONE)
- **Problem**: `deserialize_projected_from_page` did O(N×K)
  `column_projected()` scans per tuple (N=tuple cols, K=projected cols),
  and `tuple.values_.push_back(Value())` heap-allocated even for NULL.
- **Fix**: Pre-compute `source_col -> output_pos` table at plan build time,
  use direct index lookup. Skip allocation for non-projected columns.
- **Affects**: SELECT col-list FROM ... (every selective projection).

### R4 — B+tree range scan: batched leaf walk (DONE)
- **Problem**: `scan_next` re-traversed from the saved leaf id per row,
  re-fetching the page and re-decoding keys for each tuple. For a 100-row
  range that's 100 page fetches and 100 IndexKey decodes for what is at
  most one or two leaves.
- **Fix**: Index scan executor caches the current leaf page (pinned)
  across calls; advances `(leaf, slot)` cursor with single increments
  until the slot exhausts the leaf, then follows `leaf_next` once.
- **Affects**: All IndexScan / IndexOnlyScan with range predicates.

### R5 — Streaming aggregation for sorted input (DONE)
- **Problem**: AggregateExecutor always built a HashMap of groups,
  even when the input was already sorted by the GROUP BY keys (common when
  GROUP BY columns coincide with an index used by the upstream scan, or
  when a SortPlan is unavoidable for ORDER BY). Constant-memory streaming
  is asymptotically better.
- **Fix**: Optimizer marks AggregatePlan with `input_sorted` when child
  output ordering proves it. Executor takes a streaming branch with one
  `AggState` rolling forward.
- **Affects**: GROUP BY queries.

### R6 — Sort-Merge Join (DONE)
- **Problem**: HashJoin was the only choice besides O(N×M) NestedLoop.
  For pre-sorted inputs (PK on both sides, ORDER BY join key), a merge
  join is single-pass O(N+M) with zero hash table overhead.
- **Fix**: Optimizer recognises sorted-input opportunities; executor
  performs the merge.
- **Affects**: Joins on PK / indexed columns.

## Code-quality Invariants Maintained

- **Correctness**: every round ends with `ctest` 57/57 green.
- **MVCC**: visibility rules unchanged; pushed-down predicate runs *after*
  the visibility check, never before.
- **Concurrency**: no new locks; reads under `ReadGuard`, writes under
  `WriteGuard`, atomics for hot counters.
- **Code structure**: each operator owns its compiled state, no
  cross-module hidden coupling. Plan nodes describe intent; executors
  carry compiled state.
