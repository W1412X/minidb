#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: join_optimizer.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-join-opt.XXXXXX")"
SQL_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-join-opt-sql.XXXXXX")"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-join-opt-out.XXXXXX")"
trap 'rm -rf "$DB_DIR"; rm -f "$SQL_FILE" "$OUT_FILE"' EXIT

cat > "$DB_DIR/minidb.conf" <<'CFG'
shared_buffers=8MB
work_mem=1KB
statement_timeout=10s
enable_indexscan=on
enable_indexonlyscan=on
enable_hashjoin=on
CFG

{
    cat <<'SQL'
CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, status INT);
CREATE TABLE order_items (id INT PRIMARY KEY, order_id INT, product_id INT, quantity INT);
CREATE INDEX oi_order ON order_items(order_id);
SQL
    batch=""
    for i in $(seq 1 500); do
        row="($i,$((i % 50)),$((i % 3)))"
        if [[ -z "$batch" ]]; then batch="$row"; else batch="$batch,$row"; fi
        if (( i % 100 == 0 )); then
            printf 'INSERT INTO orders VALUES %s;\n' "$batch"
            batch=""
        fi
    done
    [[ -z "$batch" ]] || printf 'INSERT INTO orders VALUES %s;\n' "$batch"

    batch=""
    id=1
    for o in $(seq 1 500); do
        for k in 1 2 3; do
            row="($id,$o,$k,$((k + 1)))"
            if [[ -z "$batch" ]]; then batch="$row"; else batch="$batch,$row"; fi
            if (( id % 150 == 0 )); then
                printf 'INSERT INTO order_items VALUES %s;\n' "$batch"
                batch=""
            fi
            id=$((id + 1))
        done
    done
    [[ -z "$batch" ]] || printf 'INSERT INTO order_items VALUES %s;\n' "$batch"

    cat <<'SQL'
EXPLAIN SELECT COUNT(*) FROM orders o JOIN order_items oi ON o.id=oi.order_id WHERE o.id<100;
SELECT COUNT(*) FROM orders o JOIN order_items oi ON o.id=oi.order_id WHERE o.id<100;
SELECT COUNT(*) FROM orders o JOIN order_items oi ON o.id=oi.order_id;
ANALYZE orders;
EXPLAIN SELECT * FROM orders WHERE id BETWEEN 1 AND 5 ORDER BY id ASC;

CREATE TABLE small_a (id INT, v INT);
CREATE TABLE big_b (id INT, w INT);
SQL
    for i in $(seq 1 20); do printf 'INSERT INTO small_a VALUES (%s,%s);\n' "$i" "$i"; done
    for i in $(seq 1 20); do
        printf 'INSERT INTO big_b VALUES (%s,%s),(%s,%s);\n' "$i" "$i" "$i" "$((i + 1000))"
    done
    cat <<'SQL'
EXPLAIN SELECT COUNT(*) FROM small_a a JOIN big_b b ON a.id=b.id;
SELECT COUNT(*) FROM small_a a JOIN big_b b ON a.id=b.id;
EXPLAIN SELECT COUNT(*) FROM small_a a JOIN big_b b ON a.id<b.id;

CREATE TABLE j3 (id INT, label INT);
INSERT INTO j3 VALUES (1,10),(2,20),(3,30),(4,40);
SELECT COUNT(*) FROM small_a a JOIN big_b b ON a.id=b.id JOIN j3 c ON b.id=c.id;

SELECT o.status, COUNT(*) FROM orders o JOIN order_items oi ON o.id=oi.order_id WHERE o.id<=10 GROUP BY o.status ORDER BY o.status;
SELECT o.id, oi.quantity FROM orders o JOIN order_items oi ON o.id=oi.order_id WHERE o.id<=3 ORDER BY oi.quantity DESC LIMIT 2;
SELECT COUNT(*) FROM orders o LEFT JOIN order_items oi ON o.id=oi.order_id WHERE o.id<3;
SELECT COUNT(*) FROM orders o JOIN order_items oi ON o.id=oi.order_id WHERE o.id BETWEEN 10 AND 20;
SELECT COUNT(*) FROM orders o JOIN order_items oi ON o.id=oi.order_id WHERE o.id IN (SELECT order_id FROM order_items WHERE product_id = 1);
exit
SQL
} > "$SQL_FILE"

"$BIN" --dir "$DB_DIR" < "$SQL_FILE" > "$OUT_FILE"

if grep -qE 'Error:|failed to build plan|failed to create executor|statement timeout|unsupported or unrecognized' "$OUT_FILE"; then
    cat "$OUT_FILE" >&2
    exit 1
fi

grep -q 'IndexLookupJoin' "$OUT_FILE"
grep -q 'Filter' "$OUT_FILE"
grep -q 'IndexScan table=orders' "$OUT_FILE"
grep -q 'projected_cols=1' "$OUT_FILE"
grep -q 'ANALYZE' "$OUT_FILE"
grep -q '^297$' "$OUT_FILE"
grep -q '^1500$' "$OUT_FILE"
grep -q 'build=left' "$OUT_FILE"
grep -q '^40$' "$OUT_FILE"
grep -q 'NestedLoopJoin' "$OUT_FILE"
grep -q '^8$' "$OUT_FILE"
grep -q '^33$' "$OUT_FILE"
grep -q '^6$' "$OUT_FILE"
