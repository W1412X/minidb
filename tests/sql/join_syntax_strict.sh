#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?Usage: $0 ./build/minidb}"
DB_DIR="$(mktemp -d /tmp/minidb-join-syntax.XXXXXX)"
OUT_FILE="$(mktemp /tmp/minidb-join-syntax-out.XXXXXX)"
trap 'rm -rf "$DB_DIR"; rm -f "$OUT_FILE"' EXIT

"$BIN" --dir "$DB_DIR" > "$OUT_FILE" 2>&1 <<'SQL'
CREATE TABLE a (id INT PRIMARY KEY, v INT);
CREATE TABLE b (id INT PRIMARY KEY, v INT);
INSERT INTO a VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO b VALUES (1, 100), (2, 200);

SELECT COUNT(*) FROM a JOIN b ON a.id = b.id;
SELECT COUNT(*) FROM a INNER JOIN b ON a.id = b.id;
SELECT COUNT(*) FROM a LEFT JOIN b ON a.id = b.id;
SELECT COUNT(*) FROM a CROSS JOIN b;
SELECT COUNT(*) FROM a RIGHT JOIN b ON a.id = b.id;
SELECT COUNT(*) FROM a LEFT JOIN b;
exit
SQL

grep -q '^2$' "$OUT_FILE"
grep -q '^3$' "$OUT_FILE"
grep -q '^6$' "$OUT_FILE"

error_count="$(grep -c 'Error: unsupported or unrecognized command\.' "$OUT_FILE" || true)"
if [[ "$error_count" -ne 2 ]]; then
    cat "$OUT_FILE" >&2
    exit 1
fi
