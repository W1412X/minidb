#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: persistence_and_composite.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-persist.XXXXXX")"
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

out1="$(
    run_sql \
        'CREATE TABLE cp (a INT, b INT, c TEXT);' \
        'CREATE UNIQUE INDEX cp_ab ON cp (a, b);' \
        'INSERT INTO cp VALUES (1, 1, "x"), (1, 2, "y"), (2, 1, "z");' \
        'EXPLAIN SELECT * FROM cp WHERE a = 1 AND b = 2;' \
        'SELECT c FROM cp WHERE a = 1 AND b = 2;'
)"

require_contains 'IndexScan table=cp' "$out1"
require_contains 'c
y' "$out1"

out2="$(
    run_sql \
        'EXPLAIN SELECT * FROM cp WHERE a = 1 AND b = 2;' \
        'SELECT c FROM cp WHERE a = 1 AND b = 2;' \
        'INSERT INTO cp VALUES (1, 2, "dup");' \
        'SELECT count(*) FROM cp WHERE a = 1 AND b = 2;' \
        'EXPLAIN SELECT count(*) FROM cp WHERE a = 1;' \
        'SELECT count(*) FROM cp WHERE a = 1;'
)"

require_contains 'IndexScan table=cp' "$out2"
require_contains 'c
y' "$out2"
require_contains 'agg_0
1' "$out2"
require_contains 'agg_0
2' "$out2"

long_key="$(python3 - <<'PY'
print("x" * 600)
PY
)"

out3="$(
    run_sql \
        'CREATE TABLE names (id INT, name TEXT);' \
        'CREATE INDEX names_name ON names (name);' \
        'INSERT INTO names VALUES (1, "alice"), (2, "bob"), (3, "carol");' \
        'EXPLAIN SELECT id FROM names WHERE name = "bob";' \
        'SELECT id FROM names WHERE name = "bob";' \
        'SELECT count(*) FROM names WHERE name >= "b" AND name <= "czz";' \
        "INSERT INTO names VALUES (4, \"$long_key\");" \
        'SELECT count(*) FROM names;'
)"

require_contains 'IndexScan table=names' "$out3"
require_contains 'id
2' "$out3"
require_contains 'agg_0
2' "$out3"
require_contains 'agg_0
3' "$out3"
