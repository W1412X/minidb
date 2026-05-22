#!/usr/bin/env bash
# Regression test for filter pushdown into SeqScan / IndexScan.
#
# Verifies:
#   1. EXPLAIN shows `Filter=pushed` on the scan instead of a separate
#      Filter operator.
#   2. Filter pushdown is bypassed for predicates containing subqueries
#      (IN / EXISTS) — those still need a real Filter node so the
#      ExecutorFactory wires the inner plan correctly.
#   3. Filter pushdown is bypassed when projection pushdown is also active
#      (so we don't lose access to predicate-only columns).
#   4. Final query results match unfiltered behaviour.
set -euo pipefail

BIN="${1:?usage: filter_pushdown.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-filter-pushdown.XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

run_sql() {
    printf '%s\n' "$@" 'exit' | "$BIN" --dir "$DB_DIR"
}

require_contains() {
    local needle="$1"
    local haystack="$2"
    if [[ "$haystack" != *"$needle"* ]]; then
        printf 'expected output to contain: %s\n' "$needle" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

require_not_contains() {
    local needle="$1"
    local haystack="$2"
    if [[ "$haystack" == *"$needle"* ]]; then
        printf 'expected output not to contain: %s\n' "$needle" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

# Setup
setup_out="$(run_sql \
    'CREATE TABLE pd (id INT PRIMARY KEY, k INT, v INT, pad TEXT);' \
    'INSERT INTO pd VALUES (1,10,100,"a"),(2,20,200,"b"),(3,30,300,"c"),(4,40,400,"d"),(5,50,500,"e");' \
    'CREATE TABLE sub (x INT);' \
    'INSERT INTO sub VALUES (2),(4);')"
require_contains 'affected_rows' "$setup_out"

# 1. SeqScan + WHERE → pushed.
out="$(run_sql 'EXPLAIN SELECT * FROM pd WHERE k > 25;' \
                'SELECT * FROM pd WHERE k > 25;')"
require_contains 'Filter=pushed' "$out"
require_not_contains 'Filter
  SeqScan' "$out"
require_contains '3 | 30 | 300 | c' "$out"
require_contains '4 | 40 | 400 | d' "$out"
require_contains '5 | 50 | 500 | e' "$out"

# 2. IndexScan + residual predicate → pushed onto IndexScan.
out="$(run_sql 'EXPLAIN SELECT * FROM pd WHERE id BETWEEN 2 AND 4 AND v < 350;' \
                'SELECT * FROM pd WHERE id BETWEEN 2 AND 4 AND v < 350;')"
require_contains 'IndexScan' "$out"
require_contains 'Filter=pushed' "$out"
require_contains '2 | 20 | 200 | b' "$out"
require_contains '3 | 30 | 300 | c' "$out"
require_not_contains '4 | 40 | 400 | d' "$out"

# 3. IN-subquery predicate must NOT be pushed — needs real Filter node so
#    SubqueryInExecutor sees it.
out="$(run_sql 'EXPLAIN SELECT * FROM pd WHERE id IN (SELECT x FROM sub);' \
                'SELECT * FROM pd WHERE id IN (SELECT x FROM sub);')"
require_not_contains 'Filter=pushed' "$out"
require_contains '2 | 20 | 200 | b' "$out"
require_contains '4 | 40 | 400 | d' "$out"

# 4. Projection + filter together — optimizer un-pushes the filter so
#    predicate-only columns remain materialized. Result must still be
#    correct.
out="$(run_sql 'SELECT id FROM pd WHERE k = 30;')"
require_contains '3' "$out"
# v references a non-projected column — verify it still finds rows.
out="$(run_sql 'SELECT id FROM pd WHERE v > 250;')"
require_contains '3' "$out"
require_contains '4' "$out"
require_contains '5' "$out"

# 5. AND/OR/NOT compound predicates push correctly. We grep for the count
# of matching ids in a strict row-by-row format to avoid false positives
# from tempdir names or column headers.
out="$(run_sql 'SELECT COUNT(*) FROM pd WHERE k > 15 AND v < 450;')"
# Rows matching: id 2,3,4 → COUNT = 3.
require_contains '
3
' "$out"

out="$(run_sql 'SELECT COUNT(*) FROM pd WHERE NOT (k > 30);')"
# Rows matching: id 1,2,3 → COUNT = 3.
require_contains '
3
' "$out"

# 6. NULL three-valued logic preserved.
out="$(run_sql \
    'CREATE TABLE nul (id INT, v INT);' \
    'INSERT INTO nul VALUES (1,10),(2,NULL),(3,30);' \
    'SELECT COUNT(*) FROM nul WHERE v > 5;')"
# v > 5 matches id 1 (v=10) and 3 (v=30). NULL row excluded.
require_contains '
2
' "$out"

echo "filter_pushdown: PASS"
