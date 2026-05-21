# sql/executor

Volcano-style physical operators. `ExecutorFactory` translates an
optimised `PlanNode` tree into an `Executor` tree; the caller pulls
results by repeatedly calling `next()`.

## Executor interface

```cpp
class Executor {
public:
    virtual void   init() = 0;
    virtual ExecResult next() = 0;
    virtual const Schema& output_schema() const = 0;
    virtual bool   fast_count(u64* count);
    virtual bool   last_record_id(RecordId* rid) const;
};
```

`ExecResult` is `{has_tuple, Tuple}`. An executor returns `empty()`
to signal end-of-stream **or** failure; on failure it also calls
`set_executor_error("...")` so the caller (Repl / Server) emits an
`Error:` line.

## Implemented operators

| Operator | File | Notes |
| --- | --- | --- |
| `SeqScan` | `seq_scan.{h,cpp}` | MVCC visibility, version-chain walk, late materialisation, optional parallel scan |
| `IndexScan` / IndexOnlyScan | `index_scan_executor.{h,cpp}` | Equality + range + prefix; covering path falls back to heap recheck |
| `IndexLookupJoin` | `index_lookup_join.{h,cpp}` | Inner-index probes from a build side |
| `Filter` | `filter.{h,cpp}` | Compiled fast path for simple predicates + general evaluator |
| `Project` | `project.{h,cpp}` | Expression projection |
| `NestedLoopJoin` | `nested_loop_join.{h,cpp}` | Cross / inner / left join |
| `HashJoin` | `hash_join_executor.{h,cpp}` | Small-side build, Grace-hash spill |
| `SortExecutor` | `sort_executor.{h,cpp}` | External merge sort + Top-N heap |
| `Aggregate` | `aggregate_executor.{h,cpp}` | GROUP BY + aggregate functions + spill path |
| `Distinct` | `distinct_executor.{h,cpp}` | Hash-based dedup + spill path |
| `LimitExecutor` | `limit_executor.{h,cpp}` | LIMIT / OFFSET |
| `UnionExecutor` | `union_executor.{h,cpp}` | UNION / UNION ALL |
| `SubqueryInExecutor` | `subquery_in_executor.{h,cpp}` | IN / NOT IN on cached subquery output |
| `InsertExecutor` | `insert.{h,cpp}` | DML — surfaces NOT NULL / unique / index-insert errors |
| `UpdateExecutor` | `update.{h,cpp}` | DML — HOT path, write-write conflict detection, NOT NULL post-check |
| `DeleteExecutor` | `delete.{h,cpp}` | DML — write-write conflict detection |
| `ExpressionEvaluator` | `expression_evaluator.{h,cpp}` | Walks `Expression` ASTs against a tuple |

## ACID-relevant invariants

- **NOT NULL is loud** (C1) — `Insert/Update` call `set_executor_error("NOT NULL constraint violated")`
  and return empty rather than silently dropping the row.
- **Write-write conflicts are loud** (I2) — Update/Delete take a
  `RowExclusive` row lock, re-read `xmax`, and surface
  `could not serialize access due to concurrent update` on mismatch.
- **Heap+index atomicity** (A2) — Insert / non-HOT Update record their
  heap-insert undo BEFORE calling `db_->insert_index_entries(...)`; on a
  false return they raise `index insert failed` and let the transaction
  rollback path clean up.

## Cancellation + memory limits

- `executor_cancelled()` checks the per-statement deadline set via
  `set_executor_deadline_ms` and surfaces a `statement timeout` error
  to the caller.
- Spill-capable executors (`SortExecutor`, `HashJoin`, `Aggregate`,
  `Distinct`) cooperate with `temp_file_limit` and clean up spill files
  on normal exit and cancel paths.
