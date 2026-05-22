# record

Schema, value, and tuple serialisation — the bridge between executor-level
`Value` objects and the on-page byte layout.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `Schema` | `schema.h`, `schema.cpp` | Ordered column list + per-column metadata |
| `Column` | `schema.h`, `schema.cpp` | Name, type, NOT NULL, PRIMARY KEY, UNIQUE, DEFAULT (parsed via `default_as_value()`) |
| `Value` | `value.h`, `value.cpp` | Tagged SQL value: bool/int32/int64/float/double/varchar/null |
| `Tuple` | `tuple.h`, `tuple.cpp` | xmin/xmax/next_version + column values, serialise/deserialise to a page |

## Tuple layout on a page

```text
[ xmin: 8 ][ xmax: 8 ][ next_page: 8 ][ next_slot: 2 ][ num_cols: 4 ]
[ null_bitmap: ceil(num_cols/8) ][ column values ... ]
```

- `xmin` / `xmax` are MVCC visibility stamps.
- `next_page` / `next_slot` form the `t_ctid`-style chain pointer used
  by HOT updates.
- Each non-NULL column value is length-prefixed where needed; NULL is
  encoded purely via the bitmap.

## Default values

`Column::default_as_value()` is the single source of truth for parsing
a textual default into a typed `Value`. Called by:

- the planner when an INSERT column list omits a column,
- `Tuple::deserialize_from_page` when padding rows that pre-date an
  `ALTER TABLE ADD COLUMN` (the on-disk tuple is shorter than the
  current schema),
- `Tuple::deserialize_projected_from_page` for the projected variant.

## Invariants

- Tuples that pre-date an `ADD COLUMN` deserialize with the new column
  filled from its DEFAULT (or NULL if no default).
- `Tuple::serialize_to_page` writes a self-describing record; the reader
  uses the embedded `num_cols` rather than trusting the schema, which is
  important when the live schema has grown since the tuple was written.

## Tests

- `tests/unit/tuple_value_edge_test.cpp` — edge cases for `Tuple` /
  `Value` serialisation and comparison.
- `tests/acid/consistency/constraints_not_null_default.py` — C1 coverage
  for NOT NULL surfacing and DEFAULT substitution.
- `tests/sql/sql_correctness_matrix.py`, `tests/sql/differential_sqlite.py`
  — exercise tuple round-trip across every supported type.
