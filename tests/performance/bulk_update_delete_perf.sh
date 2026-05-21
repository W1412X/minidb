#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: bulk_update_delete_perf.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-bulk.XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

sql=(
    'CREATE TABLE bulk_t (id INT PRIMARY KEY, v INT, pad TEXT);'
)
for i in $(seq 1 1200); do
    sql+=("INSERT INTO bulk_t VALUES ($i, $i, \"pad$i\");")
done
sql+=(
    'UPDATE bulk_t SET id = id + 5000, v = v + 1 WHERE id BETWEEN 200 AND 1000;'
    'DELETE FROM bulk_t WHERE id BETWEEN 5200 AND 5600;'
    'EXPLAIN SELECT * FROM bulk_t WHERE id = 6000;'
    'SELECT id, v FROM bulk_t WHERE id = 6000;'
    'SELECT COUNT(*) FROM bulk_t;'
    'exit'
)

out="$(printf '%s\n' "${sql[@]}" | "$BIN" --dir "$DB_DIR")"

if [[ "$out" != *'IndexScan table=bulk_t'* ]]; then
    printf 'expected bulk workload lookup to still use index\n' >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
if [[ "$out" != *$'6000 | 1001'* ]]; then
    printf 'expected updated row to remain correct after bulk rewrite\n' >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
if [[ "$out" != *$'agg_0\n799'* ]]; then
    printf 'expected final row count 799 after bulk update/delete workload\n' >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
