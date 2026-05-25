#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: performance_paths.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-perf-paths.XXXXXX")"
SQL_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-perf-paths-sql.XXXXXX")"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-perf-paths-out.XXXXXX")"
trap 'rm -rf "$DB_DIR"; rm -f "$SQL_FILE" "$OUT_FILE"' EXIT

{
    cat <<'SQL'
CREATE TABLE t (id INT PRIMARY KEY, g INT, v INT);
INSERT INTO t VALUES (1,1,10),(2,1,20),(3,2,5),(4,2,7),(5,2,8);
SELECT DISTINCT g FROM t ORDER BY g DESC;
SELECT g, COUNT(*) AS c, SUM(v) AS s, AVG(v) AS a, MIN(v) AS mi, MAX(v) AS ma FROM t GROUP BY g ORDER BY g;
SELECT id FROM t UNION SELECT id FROM t ORDER BY id DESC LIMIT 3;
SELECT a.id, b.id FROM t a INNER JOIN t b ON a.g = b.g ORDER BY a.id, b.id LIMIT 6;
CREATE TABLE dup_idx (id INT PRIMARY KEY, k INT);
SQL
    batch=""
    count=0
    for i in $(seq 1 1000); do
        if [[ -z "$batch" ]]; then
            batch="($i,1)"
        else
            batch="$batch,($i,1)"
        fi
        count=$((count + 1))
        if [[ "$count" -eq 40 ]]; then
            printf 'INSERT INTO dup_idx VALUES %s;\n' "$batch"
            batch=""
            count=0
        fi
    done
    if [[ -n "$batch" ]]; then
        printf 'INSERT INTO dup_idx VALUES %s;\n' "$batch"
    fi
    cat <<'SQL'
CREATE INDEX dup_idx_k ON dup_idx(k);
EXPLAIN SELECT * FROM dup_idx WHERE k = 1;
SELECT COUNT(*) AS c FROM dup_idx WHERE k = 1;
CREATE TABLE dup_cycle (id INT PRIMARY KEY, k INT);
SQL
    batch=""
    count=0
    for i in $(seq 1 1000); do
        key=$((i % 10 + 1))
        if [[ -z "$batch" ]]; then
            batch="($i,$key)"
        else
            batch="$batch,($i,$key)"
        fi
        count=$((count + 1))
        if [[ "$count" -eq 40 ]]; then
            printf 'INSERT INTO dup_cycle VALUES %s;\n' "$batch"
            batch=""
            count=0
        fi
    done
    if [[ -n "$batch" ]]; then
        printf 'INSERT INTO dup_cycle VALUES %s;\n' "$batch"
    fi
    cat <<'SQL'
CREATE INDEX dup_cycle_k ON dup_cycle(k);
EXPLAIN SELECT COUNT(*) FROM dup_cycle WHERE k >= 3 AND k <= 7;
SELECT COUNT(*) AS c FROM dup_cycle WHERE k = 5;
SELECT COUNT(*) AS c FROM dup_cycle WHERE k >= 3 AND k <= 7;
exit
SQL
} > "$SQL_FILE"

"$BIN" --dir "$DB_DIR" < "$SQL_FILE" > "$OUT_FILE"

if grep -qE 'Error:|failed to build plan|failed to create executor|unsupported or unrecognized' "$OUT_FILE"; then
    cat "$OUT_FILE" >&2
    exit 1
fi

grep -q '^2$' "$OUT_FILE"
grep -Eq '^1 \| 2 \| 30 \| 15(\.0+)? \| 10 \| 20$' "$OUT_FILE"
grep -Eq '^2 \| 3 \| 20 \| 6(\.[0-9]+)? \| 5 \| 8$' "$OUT_FILE"
grep -q '^5$' "$OUT_FILE"
grep -q '^4$' "$OUT_FILE"
grep -q '^3$' "$OUT_FILE"
grep -q '^1 | 1$' "$OUT_FILE"
grep -q '^2 | 2$' "$OUT_FILE"
grep -q 'IndexScan table=dup_idx' "$OUT_FILE"
grep -q '^1000$' "$OUT_FILE"
grep -q 'IndexScan table=dup_cycle' "$OUT_FILE"
grep -q '^100$' "$OUT_FILE"
grep -q '^500$' "$OUT_FILE"
