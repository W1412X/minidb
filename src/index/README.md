# index

B+ tree implementation used for primary keys, unique constraints,
and explicit `CREATE INDEX` indexes.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `BPlusTree` | `btree.h`, `btree.cpp` | The B+ tree: insert / remove / search / prefix-scan / range-scan |
| `IndexKey` | `index_key.h`, `index_key.cpp` | Physical, binary-comparable key — composite-aware, null-aware, with collation slots |
| `IndexKeySchema` | `index_key.h` | Per-column type, sort direction, null ordering, collation id |
| `IndexIterator` | `index_iterator.h`, `index_iterator.cpp` | Forward iteration over leaf entries used by scan executors |

## Invariants

- **Page-local mutation** — `insert` / `remove` mutate page-locally and
  unlink empty leaves incrementally. `collect_all_entries` /
  `rebuild_from_entries` exist only on the explicit recovery / maintenance
  paths, never on hot SQL paths.
- **NULL-aware compare** — `IndexKey` rejects keys whose encoded form
  overflows the fixed slot size (`fits()` returns false); callers must
  check before insert.
- **Bytewise text collation** — strings compare via `String::compare`;
  collation id `0` means raw byte ordering. No locale-aware compare.
- **Uniqueness** — `BPlusTree::remove(key)` is valid only for unique
  trees. Non-unique deletion goes through `remove(key, rid)`. The
  catalog flag drives which API the SQL layer uses.

## Notes on covering scans

`IndexOnlyScan` uses the index data when the projected columns are a
subset of the index key, but MVCC visibility is rechecked against the
heap before the row is returned (no visibility map yet).

## Tests

- `tests/unit/btree_property_test.cpp` — randomised property tests
  against `std::multimap`.
- `tests/unit/index_key_btree_test.cpp` — composite `IndexKey` encoding
  and lookup invariants.
- `tests/index/persistence_and_composite.sh` — persistence + composite indexes.
- `tests/index/index_delete_rebuild.sh` — delete + rebuild consistency.
- `tests/index/index_unique_matrix.sh` — unique conflict matrix.
- `tests/acid/atomicity/index_heap_atomic.py` — heap/index atomicity (A2).
- `tests/acid/consistency/post_recovery_verify.py` — startup consistency check (C4).
