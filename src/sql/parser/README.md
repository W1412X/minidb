# sql/parser

Lexer, token definitions, AST node types, and a hand-written
recursive-descent parser.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `Token` / `TokenType` | `token.h` | Single tokens emitted by the lexer |
| `Lexer` | `lexer.h`, `lexer.cpp` | Splits SQL text into tokens; handles `--` line comments, string literals, keyword case-folding |
| `Parser` | `parser.h`, `parser.cpp` | Recursive-descent over the SELECT / INSERT / UPDATE / DELETE / DDL grammar |
| AST nodes (`Statement`, `Expression`, `SelectStmt`, `CreateTableStmt`, ...) | `ast.h` | Heap-allocated parse tree |

## Grammar coverage

- SELECT (incl. JOIN, GROUP BY, HAVING, ORDER BY, LIMIT/OFFSET,
  UNION/UNION ALL, DISTINCT, scalar + IN subqueries, CASE WHEN).
- INSERT (multi-row, optional column list, DEFAULT substitution at the
  planner level).
- UPDATE / DELETE with WHERE.
- DDL: `CREATE TABLE` (constraints in any order — PRIMARY KEY / NOT NULL
  / UNIQUE / DEFAULT), `DROP TABLE`, `CREATE [UNIQUE] INDEX`,
  `DROP INDEX`, `ALTER TABLE ADD/DROP/RENAME COLUMN`.
- Transactions: `BEGIN`, `COMMIT`, `ROLLBACK`.
- Prepared statements: `PREPARE`, `EXECUTE`, `DEALLOCATE`.
- Admin: `SHOW TABLES`, `DESCRIBE`, `EXPLAIN`, `EXPLAIN ANALYZE`,
  `ANALYZE`, `SHOW CONFIG`, `SHOW STATS`.
- Cursor: `DECLARE CURSOR`, `FETCH`, `CLOSE`.

## Error reporting

The parser uses `mark_error()` / `expect_*()` helpers; errors set a
parser-side flag and surface as `Error: failed to build plan` after the
planner refuses the malformed AST. Lexer errors include a position
indicator.

## Notes

- The grammar is intentionally permissive about keyword-vs-identifier
  collisions in column names (e.g. `status`); identifiers are accepted
  via `expect_alias()` where ambiguity is acceptable.
- `SET TRANSACTION ISOLATION LEVEL` is **not** parsed — see ACID I3 in
  `docs/ACID_TODO.md`. Clients cannot accidentally request SERIALIZABLE.
