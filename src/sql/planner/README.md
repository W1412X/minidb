# sql/planner

Turns the AST into a `PlanNode` tree. Each `PlanNode` is a logical
operator (SeqScan, Filter, Project, Aggregate, ...) with optional cost
metadata for the optimiser.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `Planner` | `planner.h`, `planner.cpp` | AST → PlanNode dispatch |
| `PlanNode` | `plan_node.h` | Logical operator; subtypes per node kind |

## Notable transformations

- **INSERT column-list expansion** — when the user supplies an explicit
  column list, omitted columns are filled with their `Column::default_as_value()`
  result before the planner emits the `InsertPlan`. (C1)
- **JOIN trees** — currently left-deep; the optimiser later picks
  build/probe sides for hash join, etc.
- **Projection / aggregate planning** — `rewrite_having_expr` rewrites
  HAVING expressions to reference the aggregate output columns rather
  than the source columns.
- **Subquery handling** — scalar and `IN (SELECT ...)` subqueries
  produce dedicated plan nodes; correlated subqueries are not handled
  yet.

The planner returns a null `UniquePtr<PlanNode>` on a planning failure,
which the caller surfaces as `Error: failed to build plan`.
