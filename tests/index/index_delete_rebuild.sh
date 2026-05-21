#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: index_delete_rebuild.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-index-del.XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

sql=(
    'CREATE TABLE delidx (id INT PRIMARY KEY, v INT);'
)
for i in $(seq 1 520); do
    sql+=("INSERT INTO delidx VALUES ($i, $((i * 10)));")
done
sql+=(
    'DELETE FROM delidx WHERE id BETWEEN 40 AND 480;'
    'EXPLAIN SELECT * FROM delidx WHERE id = 500;'
    'SELECT id, v FROM delidx WHERE id = 500;'
    'SELECT COUNT(*) FROM delidx;'
    'exit'
)

out="$(printf '%s\n' "${sql[@]}" | "$BIN" --dir "$DB_DIR")"

if [[ "$out" != *'IndexScan table=delidx'* ]]; then
    printf 'expected post-delete lookup to keep using index\n' >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
if [[ "$out" != *$'500 | 5000'* ]]; then
    printf 'expected row 500 to remain visible after large delete\n' >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
if [[ "$out" != *$'agg_0\n79'* ]]; then
    printf 'expected remaining row count 79 after large delete\n' >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
