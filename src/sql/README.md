# sql

Front-end + execution stack: lex / parse → plan → optimise → execute.
Subdirectories:

| Dir | Role |
| --- | --- |
| `parser/` | Lexer, token, AST, recursive-descent parser |
| `planner/` | AST → logical plan tree |
| `optimizer/` | Rule-assisted cost-based rewrites (predicate pushdown, join ordering hints, index path selection, ...) |
| `executor/` | Volcano-iterator physical operators |

See each sub-directory's README for module-level details. High-level
data flow:

```text
SQL text
   │
   ▼
parser::Parser  ─►  AST (parser/ast.h)
   │
   ▼
planner::Planner ─►  PlanNode tree (planner/plan_node.h)
   │
   ▼
optimizer        ─►  optimised PlanNode tree (same shape)
   │
   ▼
ExecutorFactory  ─►  Executor tree (executor/executor.h)
   │
   ▼
Repl / Server    ─►  output / network buffer
```

Errors from any layer are surfaced through `set_executor_error()` in
`executor/executor.h`, picked up by `Repl` / `Server` as a leading
`Error:` line.
