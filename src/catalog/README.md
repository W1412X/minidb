# catalog

Schema and statistics dictionary. Tracks tables, indexes, and the
optimiser's per-column stats. Persisted via the catalog file in the
database directory.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `Catalog` | `catalog.h`, `catalog.cpp` | Owns every `TableEntry` / `IndexEntry`. Adds / drops / renames. |
| `TableEntry` | `catalog.h` | Table id, schema, heap metadata, optimiser stats. |
| `IndexEntry` | `catalog.h` | Index id, table id, key columns, uniqueness flag, root page id. |
| `ColumnStats` | `catalog.h` | NDV, null fraction, min/max, captured by `ANALYZE`. |

## Notes

- `Catalog::for_each_table(cb, ctx)` is the read-side iteration helper
  used by `Database::flush()` / `checkpoint()` and recovery.
- Stats freshness is gated by `TableEntry::stats_valid`. Mutations
  invalidate it; `collect_statistics()` re-fills it on demand.
- The catalog file is written via `save_catalog()` / loaded via
  `load_catalog()`; both are called by `Database`.

## Tests

Catalog correctness is exercised indirectly by every DDL / DML / SQL
test. Specific catalog-touching paths:

- `tests/ddl/alter_table_semantics.sh`
- `tests/index/index_unique_matrix.sh`
- `tests/index/persistence_and_composite.sh`
