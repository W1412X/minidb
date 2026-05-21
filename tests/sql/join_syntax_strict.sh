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

# The unsupported variants ("RIGHT JOIN", "LEFT JOIN without ON") must each
# surface as a parser-level error. The exact wording is "syntax error" or
# "expected ...", so accept any error line that is not table-not-found.
error_count="$(grep -cE '^Error: (syntax error|expected |unknown statement|unexpected trailing|only SHOW)' "$OUT_FILE" || true)"
if [[ "$error_count" -ne 2 ]]; then
    cat "$OUT_FILE" >&2
    exit 1
fi
