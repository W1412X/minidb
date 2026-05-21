#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: performance_optimizations.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-perf-opts.XXXXXX")"
SQL_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-perf-opts-sql.XXXXXX")"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-perf-opts-out.XXXXXX")"
trap 'rm -rf "$DB_DIR"; rm -f "$SQL_FILE" "$OUT_FILE"' EXIT

cat > "$DB_DIR/minidb.conf" <<'CFG'
shared_buffers=4MB
work_mem=256KB
enable_indexscan=on
enable_indexonlyscan=on
enable_parallel_seqscan=on
parallel_workers=4
buffer_pool_partitions=8
wal_fsync=on
wal_group_commit=on
wal_group_commit_delay=2
gc_enabled=on
gc_ops_threshold=1
CFG

{
    cat <<'SQL'
CREATE TABLE tn (id INT PRIMARY KEY, score INT);
SQL
    batch=""
    count=0
    for i in $(seq 1 240); do
        row="($i,$i)"
        if [[ -z "$batch" ]]; then batch="$row"; else batch="$batch,$row"; fi
        count=$((count + 1))
        if [[ "$count" -eq 40 ]]; then
            printf 'INSERT INTO tn VALUES %s;\n' "$batch"
            batch=""
            count=0
        fi
    done
    if [[ -n "$batch" ]]; then printf 'INSERT INTO tn VALUES %s;\n' "$batch"; fi
    cat <<'SQL'
SELECT id FROM tn ORDER BY score DESC LIMIT 5;
SELECT COUNT(*) AS c FROM tn WHERE (id >= 10 AND id <= 20) OR id = 240;
PREPARE fast_count AS SELECT COUNT(*) AS c FROM tn WHERE id >= 100 AND id <= 120;
EXECUTE fast_count;
DEALLOCATE fast_count;
CREATE TABLE wide (c1 INT, c2 INT, c3 INT, c4 INT, c5 INT, c6 INT, c7 INT, c8 INT, c9 INT, c10 INT, c11 INT, c12 INT, c13 INT, c14 INT, c15 INT, c16 INT, c17 INT, c18 INT, c19 INT, c20 INT);
INSERT INTO wide VALUES (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20),(21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40);
EXPLAIN SELECT c1, c20 FROM wide;
SELECT c1, c20 FROM wide ORDER BY c1;
UPDATE tn SET score = 1000 WHERE id = 1;
UPDATE tn SET score = 1001 WHERE id = 1;
UPDATE tn SET score = 1002 WHERE id = 1;
SELECT score FROM tn WHERE id = 1;
SHOW STATS;
exit
SQL
} > "$SQL_FILE"

"$BIN" --dir "$DB_DIR" < "$SQL_FILE" > "$OUT_FILE"

if grep -qE 'Error:|failed to build plan|failed to create executor|unsupported or unrecognized' "$OUT_FILE"; then
    cat "$OUT_FILE" >&2
    exit 1
fi

grep -q '^240$' "$OUT_FILE"
grep -q '^239$' "$OUT_FILE"
grep -q '^238$' "$OUT_FILE"
grep -q '^237$' "$OUT_FILE"
grep -q '^236$' "$OUT_FILE"
grep -q '^12$' "$OUT_FILE"
grep -q '^21$' "$OUT_FILE"
grep -q '^SeqScan table=wide' "$OUT_FILE"
grep -q '^1 | 20$' "$OUT_FILE"
grep -q '^21 | 40$' "$OUT_FILE"
grep -q '^1002$' "$OUT_FILE"
grep -q '^buffer_pool_partitions=8$' "$OUT_FILE"
grep -q '^wal_buffer_flushes=' "$OUT_FILE"
grep -q '^wal_buffered_bytes=' "$OUT_FILE"
