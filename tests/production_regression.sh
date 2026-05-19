#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: production_regression.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-prod-reg.XXXXXX")"
SPILL_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-prod-spill.XXXXXX")"
SQL_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-prod-reg-sql.XXXXXX")"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-prod-reg-out.XXXXXX")"
trap 'rm -rf "$DB_DIR" "$SPILL_DIR"; rm -f "$SQL_FILE" "$OUT_FILE"' EXIT

cat > "$DB_DIR/minidb.conf" <<CFG
shared_buffers=1MB
work_mem=1KB
temp_dir=$SPILL_DIR
statement_timeout=10s
enable_hashjoin=on
enable_indexscan=on
enable_indexonlyscan=on
CFG

{
    cat <<'SQL'
CREATE TABLE l (id INT PRIMARY KEY, v INT);
CREATE TABLE r (id INT PRIMARY KEY, v INT);
INSERT INTO l VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO r VALUES (1, 100), (2, 200);
SELECT COUNT(*) FROM l LEFT JOIN r ON l.id = r.id WHERE r.v = 999;
SELECT COUNT(*) FROM l LEFT JOIN r ON l.id = r.id WHERE l.id < 3;
SELECT COUNT(*) FROM l CROSS JOIN r;

CREATE TABLE wide (id INT PRIMARY KEY, g INT, payload VARCHAR(80));
CREATE TABLE fact (id INT PRIMARY KEY, wide_id INT, amount INT);
SQL
    batch=""
    for i in $(seq 1 240); do
        row="($i,$((i % 17)),'payload-$i-payload-$i-payload-$i')"
        if [[ -z "$batch" ]]; then batch="$row"; else batch="$batch,$row"; fi
        if (( i % 40 == 0 )); then
            printf 'INSERT INTO wide VALUES %s;\n' "$batch"
            batch=""
        fi
    done
    [[ -z "$batch" ]] || printf 'INSERT INTO wide VALUES %s;\n' "$batch"

    batch=""
    for i in $(seq 1 720); do
        row="($i,$(( (i % 240) + 1 )),$((i % 23)))"
        if [[ -z "$batch" ]]; then batch="$row"; else batch="$batch,$row"; fi
        if (( i % 80 == 0 )); then
            printf 'INSERT INTO fact VALUES %s;\n' "$batch"
            batch=""
        fi
    done
    [[ -z "$batch" ]] || printf 'INSERT INTO fact VALUES %s;\n' "$batch"

    cat <<'SQL'
ANALYZE wide;
EXPLAIN SELECT g, COUNT(*) FROM wide GROUP BY g ORDER BY g;
SELECT g, COUNT(*) FROM wide GROUP BY g ORDER BY g;
SELECT DISTINCT payload FROM wide ORDER BY payload LIMIT 5;
SELECT COUNT(*) FROM wide w JOIN fact f ON w.id = f.wide_id;
EXPLAIN SELECT COUNT(*) FROM wide w JOIN fact f ON w.id = f.wide_id WHERE w.id < 20;
exit
SQL
} > "$SQL_FILE"

"$BIN" --dir "$DB_DIR" < "$SQL_FILE" > "$OUT_FILE"

if grep -qE 'Error:|failed to build plan|failed to create executor|statement timeout|unsupported or unrecognized' "$OUT_FILE"; then
    cat "$OUT_FILE" >&2
    exit 1
fi

grep -q '^0$' "$OUT_FILE"
grep -q '^2$' "$OUT_FILE"
grep -q '^6$' "$OUT_FILE"
grep -q '^720$' "$OUT_FILE"
grep -q 'HashJoin' "$OUT_FILE"
grep -q 'Filter' "$OUT_FILE"

if find "$SPILL_DIR" -type f -name 'minidb_*' | grep -q .; then
    find "$SPILL_DIR" -type f -name 'minidb_*' >&2
    exit 1
fi
