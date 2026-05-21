#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: performance_delivery.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-perf-delivery.XXXXXX")"
SQL_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-perf-delivery-sql.XXXXXX")"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-perf-delivery-out.XXXXXX")"
trap 'rm -rf "$DB_DIR"; rm -f "$SQL_FILE" "$OUT_FILE"' EXIT

cat > "$DB_DIR/minidb.conf" <<'CFG'
shared_buffers=2MB
work_mem=1MB
enable_indexscan=on
enable_indexonlyscan=on
enable_parallel_seqscan=on
parallel_workers=4
buffer_pool_partitions=8
wal_fsync=on
wal_group_commit=on
wal_group_commit_delay=1
gc_enabled=on
gc_ops_threshold=10
CFG

{
    cat <<'SQL'
SHOW CONFIG;
CREATE TABLE t (id INT PRIMARY KEY, g INT, pad TEXT);
SQL
    batch=""
    count=0
    for i in $(seq 1 3000); do
        row="($i,$((i % 7)),\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        if [[ -z "$batch" ]]; then
            batch="$row"
        else
            batch="$batch,$row"
        fi
        count=$((count + 1))
        if [[ "$count" -eq 40 ]]; then
            printf 'INSERT INTO t VALUES %s;\n' "$batch"
            batch=""
            count=0
        fi
    done
    if [[ -n "$batch" ]]; then
        printf 'INSERT INTO t VALUES %s;\n' "$batch"
    fi
    cat <<'SQL'
EXPLAIN SELECT id FROM t WHERE id = 42;
SELECT id FROM t WHERE id = 42;
SELECT COUNT(*) AS c FROM t;
PREPARE p AS SELECT id FROM t WHERE id = 77;
EXECUTE p;
DEALLOCATE p;
UPDATE t SET g = 9 WHERE id = 42;
UPDATE t SET g = 10 WHERE id = 42;
UPDATE t SET g = 11 WHERE id = 42;
SELECT id, g FROM t WHERE id = 42;
SHOW STATS;
exit
SQL
} > "$SQL_FILE"

"$BIN" --dir "$DB_DIR" < "$SQL_FILE" > "$OUT_FILE"

if grep -qE 'Error:|failed to build plan|failed to create executor|unsupported or unrecognized' "$OUT_FILE"; then
    cat "$OUT_FILE" >&2
    exit 1
fi

grep -q 'enable_indexonlyscan=on' "$OUT_FILE"
grep -q 'enable_parallel_seqscan=on' "$OUT_FILE"
grep -q 'buffer_pool_partitions=8' "$OUT_FILE"
grep -q 'wal_group_commit=on' "$OUT_FILE"
grep -q 'IndexOnlyScan table=t' "$OUT_FILE"
grep -q '^42$' "$OUT_FILE"
grep -q '^3000$' "$OUT_FILE"
grep -q '^77$' "$OUT_FILE"
grep -q '^42 | 11$' "$OUT_FILE"
grep -q '^wal_group_commit_batches=' "$OUT_FILE"
grep -q '^buffer_pool_partitions=8$' "$OUT_FILE"
