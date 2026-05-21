#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: hot_index_semantics.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-hot.XXXXXX")"
SQL_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-hot-sql.XXXXXX")"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-hot-out.XXXXXX")"
trap 'rm -rf "$DB_DIR"; rm -f "$SQL_FILE" "$OUT_FILE"' EXIT

cat > "$SQL_FILE" <<'SQL'
CREATE TABLE h (id INT PRIMARY KEY, v INT, pad TEXT);
INSERT INTO h VALUES (1, 10, "a");
INSERT INTO h VALUES (2, 20, "b");
EXPLAIN SELECT v FROM h WHERE id = 1;
UPDATE h SET v = 11 WHERE id = 1;
SELECT v FROM h WHERE id = 1;
SELECT id FROM h WHERE id = 1;
SELECT COUNT(*) FROM h WHERE id = 1;
BEGIN;
UPDATE h SET v = 12 WHERE id = 1;
SELECT v FROM h WHERE id = 1;
ROLLBACK;
SELECT v FROM h WHERE id = 1;
SELECT COUNT(*) FROM h WHERE id = 1;
UPDATE h SET id = 10 WHERE id = 1;
SELECT id, v FROM h WHERE id = 10;
SELECT COUNT(*) FROM h WHERE id = 1;
SELECT COUNT(*) FROM h WHERE id = 10;
exit
SQL

"$BIN" --dir "$DB_DIR" < "$SQL_FILE" > "$OUT_FILE"

if grep -qE 'Error:|failed to build plan|failed to create executor|unsupported or unrecognized' "$OUT_FILE"; then
    cat "$OUT_FILE" >&2
    exit 1
fi

grep -q 'IndexScan table=h' "$OUT_FILE"
grep -q '^11$' "$OUT_FILE"
grep -q '^12$' "$OUT_FILE"
grep -q '^10 | 11$' "$OUT_FILE"

count_ones="$(grep -c '^1$' "$OUT_FILE")"
if [[ "$count_ones" -lt 4 ]]; then
    printf 'expected HOT index lookups/counts to keep one visible row\n' >&2
    cat "$OUT_FILE" >&2
    exit 1
fi

grep -q $'agg_0\n0' "$OUT_FILE"
