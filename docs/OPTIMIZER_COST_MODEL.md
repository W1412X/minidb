# Optimizer Cost Model

MiniDB uses a rule-assisted cost model. Rules apply semantic rewrites first; costs choose among access and join alternatives.

## Statistics

`ANALYZE` persists table-level row/page counts and per-column statistics:

- NDV
- null count
- numeric min/max

Statistics are considered fresh when analyzed rows are within 20% of current table row count. Stale or missing statistics fall back to current catalog row/page counts and conservative selectivity defaults.

NDV collection uses typed value encoding, so `VARCHAR`, numeric, boolean, and null values do not collapse into the same key. Exact NDV collection is capped per column to bound memory use on very high-cardinality columns.

## Selectivity

- Equality predicates use `1 / NDV` when fresh stats exist.
- Range predicates use min/max when available.
- Unknown predicates fall back to conservative defaults.
- Join equality selectivity uses `1 / max(left_ndv, right_ndv)` after clamping NDV to side cardinality.
- Cross joins use full Cartesian cardinality.
- Left joins preserve at least left-side cardinality in estimates.

## Plan Choices

The optimizer can choose:

- sequential scan
- index scan / index-only scan
- hash join with build-side choice
- index lookup join when the inner side has a usable index
- nested loop only when no better equi-join strategy applies
- projection and predicate pushdown where semantics allow it

For `LEFT JOIN`, right-side `WHERE` predicates are not pushed below the join because that would change null-extension semantics.

## Remote Storage Costing

When remote storage mode is enabled, scan and lookup costs include remote page and round-trip constants. Random index lookups are therefore not always preferred over sequential access.

## Current Boundaries

- Multi-column MCV/histogram statistics are not complete.
- Adaptive execution feedback is not yet used to retrain costs.
- Bushy join enumeration is limited compared with production CBOs.
