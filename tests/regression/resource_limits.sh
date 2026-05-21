#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?Usage: $0 ./build/minidb}"
DB_DIR="$(mktemp -d /tmp/minidb_resource_limits.XXXXXX)"
SQL_FILE="$(mktemp /tmp/minidb_resource_limits_sql.XXXXXX)"
CONF_FILE="$(mktemp /tmp/minidb_resource_limits_conf.XXXXXX)"
trap 'rm -rf "$DB_DIR" "$SQL_FILE" "$CONF_FILE"' EXIT

cat > "$CONF_FILE" <<'CONF'
work_mem=1KB
query_memory_limit=2MB
temp_file_limit=64MB
temp_dir=/tmp
max_active_queries=2
max_active_write_queries=1
max_active_transactions=4
buffer_pool_wait_timeout=500MS
max_buffer_waiters=16
statement_timeout=10S
CONF

{
    echo 'CREATE TABLE t (id INT, v INT);'
    i=160
    while [ "$i" -ge 1 ]; do
        echo "INSERT INTO t VALUES ($i, $((i * 10)));"
        i=$((i - 1))
    done
    echo 'SELECT id FROM t ORDER BY id;'
    echo 'SELECT DISTINCT v FROM t ORDER BY v;'
    echo 'SELECT v, COUNT(*) FROM t GROUP BY v ORDER BY v;'
    echo 'CREATE TABLE u (id INT, w INT);'
    i=1
    while [ "$i" -le 40 ]; do
        echo "INSERT INTO u VALUES ($i, $((i * 100)));"
        i=$((i + 1))
    done
    echo 'SELECT t.id, u.w FROM t INNER JOIN u ON t.id = u.id WHERE t.id < 4 ORDER BY t.id;'
    echo 'SHOW CONFIG;'
    echo 'SHOW STATS;'
    echo 'exit;'
} > "$SQL_FILE"

OUT="$("$BIN" --config "$CONF_FILE" --dir "$DB_DIR" < "$SQL_FILE" 2>&1)"

printf '%s\n' "$OUT" | grep -q '^1$'
printf '%s\n' "$OUT" | grep -q '^160$'
printf '%s\n' "$OUT" | grep -q '^1 | 100$'
printf '%s\n' "$OUT" | grep -q '^2 | 200$'
printf '%s\n' "$OUT" | grep -q '^3 | 300$'
if printf '%s\n' "$OUT" | grep -q 'work_mem exceeded'; then
    printf '%s\n' "$OUT" >&2
    exit 1
fi
printf '%s\n' "$OUT" | grep -q '^query_memory_limit=2097152$'
printf '%s\n' "$OUT" | grep -q '^max_active_transactions=4$'
printf '%s\n' "$OUT" | grep -q '^buffer_pool_wait_timeout=500$'
printf '%s\n' "$OUT" | grep -q '^active_queries=0$'
printf '%s\n' "$OUT" | grep -q '^active_transactions=0$'
printf '%s\n' "$OUT" | grep -q '^buffer_waiters=0$'

cat > "$CONF_FILE" <<'CONF'
work_mem=1KB
temp_file_limit=1KB
temp_dir=/tmp
statement_timeout=10S
CONF

OUT="$("$BIN" --config "$CONF_FILE" --dir "$DB_DIR.limit" < "$SQL_FILE" 2>&1 || true)"
printf '%s\n' "$OUT" | grep -q 'temp_file_limit exceeded during sort'
