#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: sql_regression.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-regression.XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

run_sql() {
    printf '%s\n' "$@" 'exit' | "$BIN" --dir "$DB_DIR"
}

require_contains() {
    local needle="$1"
    local haystack="$2"
    if [[ "$haystack" != *"$needle"* ]]; then
        printf 'expected output to contain: %s\n' "$needle" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

require_not_contains() {
    local needle="$1"
    local haystack="$2"
    if [[ "$haystack" == *"$needle"* ]]; then
        printf 'expected output not to contain: %s\n' "$needle" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

out="$(
    run_sql \
        'CREATE TABLE t (id INT PRIMARY KEY, email TEXT UNIQUE, v INT NOT NULL);' \
        'INSERT INTO t VALUES (1, "a", 10), (2, "b", 20), (3, "c", 30);' \
        'EXPLAIN SELECT * FROM t WHERE id = 2;' \
        'SELECT * FROM t WHERE id = 2;' \
        'EXPLAIN SELECT * FROM t WHERE id BETWEEN 1 AND 2;' \
        'SELECT id FROM t WHERE id BETWEEN 1 AND 2 ORDER BY id DESC;' \
        'INSERT INTO t VALUES (2, "dup", 99);' \
        'INSERT INTO t VALUES (4, "b", 99);' \
        'EXPLAIN UPDATE t SET id = 4, v = 40 WHERE id = 2;' \
        'UPDATE t SET id = 4, v = 40 WHERE id = 2;' \
        'SELECT id, v FROM t WHERE id = 4;' \
        'EXPLAIN DELETE FROM t WHERE id = 4;' \
        'DELETE FROM t WHERE id = 4;' \
        'SELECT id FROM t ORDER BY id;' \
        'CREATE INDEX idx_v ON t (v);' \
        'EXPLAIN SELECT * FROM t WHERE v = 30;' \
        'DROP INDEX idx_v;' \
        'EXPLAIN SELECT * FROM t WHERE v = 30;' \
        'EXPLAIN SELECT * FROM t WHERE v = 30 AND id = 3;' \
        'CREATE TABLE rt (id INT PRIMARY KEY, v INT);' \
        'INSERT INTO rt VALUES (6, 60), (7, 70), (8, 80);' \
        'EXPLAIN UPDATE rt SET id = id + 100, v = v + 1 WHERE id BETWEEN 6 AND 8;' \
        'UPDATE rt SET id = id + 100, v = v + 1 WHERE id BETWEEN 6 AND 8;' \
        'SELECT id, v FROM rt WHERE id = 107;' \
        'SELECT COUNT(*), SUM(v), AVG(v), MIN(v), MAX(v) FROM t;' \
        'CREATE TABLE u (id INT, w INT);' \
        'INSERT INTO u VALUES (1, 100), (3, 300);' \
        'EXPLAIN SELECT t.id, u.w FROM t JOIN u ON t.id = u.id;' \
        'SELECT t.id, u.w FROM t LEFT JOIN u ON t.id = u.id ORDER BY t.id;' \
        'SELECT id FROM t UNION ALL SELECT id FROM u ORDER BY id LIMIT 5 OFFSET 1;' \
        'SELECT id FROM t WHERE id IN (SELECT id FROM u) ORDER BY id;' \
        'SELECT id, CASE WHEN v > 10 THEN "hi" ELSE "lo" END FROM t ORDER BY id;' \
        'SELECT id FROM t WHERE email LIKE "c%";' \
        'BEGIN;' \
        'INSERT INTO t VALUES (5, "e", 50);' \
        'EXPLAIN SELECT * FROM t WHERE id = 5;' \
        'SELECT id FROM t WHERE id = 5;' \
        'ROLLBACK;' \
        'SELECT id FROM t WHERE id = 5;'
)"

require_contains 'IndexScan table=t index=1000 key=2' "$out"
require_contains 'Plan: cost=' "$out"
require_contains 'IndexScan table=t index=1000 range=[1,2]' "$out"
require_contains 'Update
  IndexScan table=t index=1000 key=2' "$out"
require_contains 'Update
  IndexScan table=rt' "$out"
require_contains 'range=[6,8]' "$out"
require_contains 'Delete
  IndexScan table=t index=1000 key=4' "$out"
require_contains 'IndexScan table=t index=1002 key=30' "$out"
# Filter is now pushed directly into the scan operator — the executor pipeline
# no longer instantiates a separate FilterExecutor when the predicate sits
# atop a SeqScan/IndexScan. EXPLAIN still surfaces the predicate via the
# `Filter=pushed` annotation on the scan line.
require_contains 'SeqScan table=t' "$out"
require_contains 'Filter=pushed' "$out"
require_contains 'IndexScan table=t index=1000 key=3' "$out"
require_contains 'HashJoin cost=' "$out"
require_contains '2 | b | 20' "$out"
require_contains '4 | 40' "$out"
require_contains '107 | 71' "$out"
require_contains '2 | 40 | 20' "$out"
require_contains '1 | 100' "$out"
require_contains '3 | 300' "$out"
require_contains '3 | hi' "$out"
require_contains 'Transaction rolled back.' "$out"

union_out="$(run_sql 'SELECT id FROM t UNION SELECT id FROM t ORDER BY id;')"
union_count=$(printf '%s\n' "$union_out" | awk '/^[0-9]+$/ { count++ } END { print count + 0 }')
if [[ "$union_count" != "2" ]]; then
    printf 'expected UNION distinct to emit exactly 2 ids, got %s\n' "$union_count" >&2
    printf '%s\n' "$union_out" >&2
    exit 1
fi

out2="$(
    run_sql \
        'SHOW TABLES;' \
        'DESC t;' \
        'EXPLAIN SELECT * FROM t WHERE id = 3;' \
        'SELECT * FROM t WHERE id = 3;' \
        'INSERT INTO t VALUES (3, "z", 99);' \
        'INSERT INTO t VALUES (6, "c", 99);' \
        'INSERT INTO t VALUES (6, "f", 60);' \
        'SELECT id, email FROM t ORDER BY id;'
)"

require_contains 'Tables:' "$out2"
require_contains 'Unique' "$out2"
require_contains 'IndexScan table=t index=1000 key=3' "$out2"
require_contains '3 | c | 30' "$out2"
require_contains '6 | f' "$out2"
require_not_contains '6 | c' "$out2"

order_out="$(
    run_sql \
        'CREATE TABLE ordx (id INT, v INT);' \
        'INSERT INTO ordx VALUES (1, 30), (2, 10), (3, 20);' \
        'SELECT id FROM ordx ORDER BY v DESC;' \
        'SELECT id + 10 FROM ordx ORDER BY v;'
)"
require_contains 'id
1
3
2' "$order_out"
require_contains 'expr_0
12
13
11' "$order_out"

extra_out="$(
    run_sql \
        'CREATE TABLE uq (a INT, b INT, c TEXT);' \
        'CREATE UNIQUE INDEX uq_ab ON uq (a, b);' \
        'INSERT INTO uq VALUES (1, 1, "x"), (1, 2, "y"), (NULL, 1, "n1"), (NULL, 1, "n2");' \
        'INSERT INTO uq VALUES (1, 1, "dup");' \
        'UPDATE uq SET c = "z" WHERE a = 1;' \
        'SELECT c FROM uq WHERE a = 1 AND b = 2;' \
        'SELECT COUNT(*) FROM uq;' \
        'CREATE TABLE alias_t (id INT PRIMARY KEY, v INT);' \
        'INSERT INTO alias_t VALUES (1, 10), (2, 20);' \
        'EXPLAIN SELECT a.id FROM alias_t a WHERE a.id = 2;' \
        'SELECT a.id FROM alias_t a WHERE a.id = 2;'
)"
require_contains 'affected_rows
0' "$extra_out"
require_contains 'c
z' "$extra_out"
require_contains 'agg_0
4' "$extra_out"
require_contains 'IndexOnlyScan table=alias_t' "$extra_out"
require_contains 'id
2' "$extra_out"

alias_agg_out="$(
    run_sql \
        'SELECT COUNT(a.id) FROM alias_t a;' \
        'SELECT COUNT(*) alias_count;' \
        'CREATE TABLE alias_orders (id INT PRIMARY KEY, customer_id INT, total DECIMAL(10,2));' \
        'CREATE TABLE alias_items (id INT PRIMARY KEY, order_id INT, quantity INT, unit_price DECIMAL(10,2));' \
        'INSERT INTO alias_orders VALUES (1, 10, 99.5), (2, 11, 30.0);' \
        'INSERT INTO alias_items VALUES (1, 1, 2, 49.75), (2, 2, 3, 10.0), (3, 1, 1, 49.75);' \
        'SELECT COUNT(o.id) FROM alias_orders o;' \
        'SELECT COUNT(oi.id) FROM alias_items oi;' \
        'SELECT * FROM alias_orders o JOIN alias_items oi ON o.id = oi.order_id LIMIT 5;' \
        'SELECT * FROM alias_items oi JOIN alias_orders o ON o.id = oi.order_id LIMIT 5;'
)"
require_contains 'agg_0
2' "$alias_agg_out"
require_contains 'alias_count
1' "$alias_agg_out"
require_contains 'agg_0
3' "$alias_agg_out"
require_contains 'id | customer_id | total | id | order_id | quantity | unit_price' "$alias_agg_out"
require_contains '1 | 10 | 99.500000 | 1 | 1 | 2 | 49.750000' "$alias_agg_out"
require_contains 'id | order_id | quantity | unit_price | id | customer_id | total' "$alias_agg_out"
require_contains '1 | 1 | 2 | 49.750000 | 1 | 10 | 99.500000' "$alias_agg_out"
require_not_contains 'unexpected token' "$alias_agg_out"

onerow_out="$(
    run_sql \
        'EXPLAIN SELECT 1 + 2;' \
        'SELECT 1 + 2;' \
        'SELECT NULL OR TRUE;' \
        'SELECT 1 WHERE TRUE;' \
        'SELECT 1 WHERE FALSE;' \
        'CREATE TABLE limit_neg (id INT PRIMARY KEY);' \
        'INSERT INTO limit_neg VALUES (1), (2), (3);' \
        'SELECT id FROM limit_neg ORDER BY id LIMIT -1;' \
        'SELECT COUNT(*);' \
        'CREATE TABLE lower_count (id INT);' \
        'INSERT INTO lower_count VALUES (1), (2);' \
        'select count(*) from lower_count;' \
        'select count(*) as count from lower_count;' \
        'SELECT 3 UNION SELECT 3;'
)"
require_contains 'Project
  OneRow' "$onerow_out"
require_contains 'expr_0
3' "$onerow_out"
require_contains 'expr_0
1' "$onerow_out"
require_contains 'id
1
2
3' "$onerow_out"
require_contains 'agg_0
1' "$onerow_out"
require_contains 'agg_0
2' "$onerow_out"
require_contains 'count
2' "$onerow_out"

case_out="$(
    run_sql \
        'CrEaTe TaBlE CaseMix (Id inT PrImArY kEy, Flag BoOl, Name VaRcHaR);' \
        'InSeRt InTo CaseMix VaLuEs (1, TrUe, "Alice"), (2, FaLsE, "Bob");' \
        'sElEcT CoUnT(*) FrOm CaseMix WhErE Flag Is NoT NuLl;' \
        'eXpLaIn sElEcT * fRoM CaseMix wHeRe Id = 2;' \
        'sElEcT Name FrOm CaseMix WhErE Id != 1 oRdEr By Id dEsC;'
)"
require_contains 'agg_0
2' "$case_out"
require_contains 'IndexScan table=CaseMix' "$case_out"
require_contains 'Name
Bob' "$case_out"

parser_guard_out="$(
    run_sql \
        'CREATE TABLE bad_parser_artifact (id INT,);' \
        'CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, store_id INT, date_id INT, total DECIMAL(10,2));' \
        'CREATE TABLE order_items (id INT PRIMARY KEY, order_id INT, product_id INT, quantity INT, unit_price DECIMAL(10,2));' \
        'INSERT INTO orders VALUES (1, 10, 20, 30, 99.5);' \
        'INSERT INTO order_items VALUES (1, 1, 100, 2, 49.75);' \
        'SHOW TABLES;' \
        'DESC orders;' \
        'SELECT * FROM order_items oi JOIN orders o ON o.id = oi.order_id LIMIT 50;'
)"
require_not_contains 'bad_parser_artifact' "$parser_guard_out"
require_not_contains 'unexpected token' "$parser_guard_out"
require_contains 'unit_price | id | customer_id | store_id | date_id | total' "$parser_guard_out"
require_contains '49.750000 | 1 | 10 | 20 | 30 | 99.500000' "$parser_guard_out"

big_sql=('CREATE TABLE bigidx (id INT PRIMARY KEY, v INT);')
for i in $(seq 1 430); do
    big_sql+=("INSERT INTO bigidx VALUES ($i, $((i * 2)));")
done
big_sql+=(
    'EXPLAIN SELECT * FROM bigidx WHERE id = 425;'
    'SELECT id, v FROM bigidx WHERE id = 425;'
    'SELECT COUNT(*), SUM(id) FROM bigidx;'
    'SELECT id FROM bigidx WHERE id BETWEEN 398 AND 405 ORDER BY id;'
    'UPDATE bigidx SET id = 501, v = 1002 WHERE id = 425;'
    'SELECT id, v FROM bigidx WHERE id = 501;'
    'SELECT id FROM bigidx WHERE id = 425;'
    'DELETE FROM bigidx WHERE id BETWEEN 400 AND 405;'
    'SELECT COUNT(*) FROM bigidx;'
)
big_out="$(run_sql "${big_sql[@]}")"
require_contains 'IndexScan table=bigidx' "$big_out"
require_contains 'key=425' "$big_out"
require_contains '425 | 850' "$big_out"
require_contains '430 | 92665' "$big_out"
require_contains '398' "$big_out"
require_contains '405' "$big_out"
require_contains '501 | 1002' "$big_out"
require_contains 'deleted_rows
6' "$big_out"
require_contains '424' "$big_out"

# Double-precision literals must not be narrowed to 32-bit float by the parser:
# a DOUBLE column value must compare equal to the same literal, and a literal
# that only differs beyond float precision must NOT match.
dbl_out="$(
    run_sql \
        'CREATE TABLE dbl_lit (id INT PRIMARY KEY, d DOUBLE);' \
        'INSERT INTO dbl_lit VALUES (1, 3.141592653589793);' \
        'SELECT id FROM dbl_lit WHERE d = 3.141592653589793;' \
        'SELECT COUNT(*) FROM dbl_lit WHERE d = 3.1415927;'
)"
require_contains 'id
1' "$dbl_out"
require_contains 'agg_0
0' "$dbl_out"

# SUM/AVG over an INT32 column must accumulate in 64-bit. Each value is in
# int32 range but the running total overflows int32; the result must not
# collapse to NULL. Covers both the fast (no GROUP BY) and slow (GROUP BY)
# aggregation paths.
sum_out="$(
    run_sql \
        'CREATE TABLE sumbig (g INT, x INT);' \
        'INSERT INTO sumbig VALUES (1, 2000000000), (1, 2000000000), (2, 2000000000), (2, 2000000000);' \
        'SELECT SUM(x) FROM sumbig;' \
        'SELECT g, SUM(x) FROM sumbig GROUP BY g ORDER BY g;'
)"
require_contains 'agg_0
8000000000' "$sum_out"
require_contains '1 | 4000000000' "$sum_out"
require_not_contains 'NULL' "$sum_out"

# COUNT(*) over a join with a single-table WHERE: the predicate is pushed into
# the scan, and the count-only late-materialization projection must not drop the
# column the pushed predicate references (else every row is filtered out → 0).
cntpred_out="$(
    run_sql \
        'CREATE TABLE cpa (id INT, x INT);' \
        'CREATE TABLE cpb (id INT);' \
        'INSERT INTO cpa VALUES (1,50),(2,150),(3,200),(4,80);' \
        'INSERT INTO cpb VALUES (1),(2),(3),(4);' \
        'SELECT COUNT(*) FROM cpa JOIN cpb ON cpa.id = cpb.id WHERE cpa.x > 100;' \
        'SELECT COUNT(*) FROM cpa JOIN cpb ON cpa.id = cpb.id;'
)"
require_contains 'agg_0
2' "$cntpred_out"
require_contains 'agg_0
4' "$cntpred_out"

# A column DEFAULT must be stored with the column's exact type, not widened.
# A widened default (int64 for an INT32 column) compares unequal to a same-typed
# literal because Value::compare orders by type-id first.
def_out="$(
    run_sql \
        'CREATE TABLE deftype (id INT PRIMARY KEY, a INT DEFAULT 5);' \
        'INSERT INTO deftype (id) VALUES (1);' \
        'INSERT INTO deftype VALUES (2, 9);' \
        'SELECT id FROM deftype WHERE a = 5;' \
        'SELECT COUNT(*) FROM deftype WHERE a = 5;'
)"
require_contains 'id
1' "$def_out"
require_contains 'agg_0
1' "$def_out"

# Compiled predicate must compare cross-type numerics by value, matching the
# interpreter. A genuine int64 value (via CAST) compared to an int32 literal
# must still match — raw Value::compare would order by type-id and miss it.
xtype_out="$(
    run_sql \
        'CREATE TABLE xt (id BIGINT);' \
        'INSERT INTO xt VALUES (CAST(3 AS BIGINT)), (CAST(8 AS BIGINT));' \
        'SELECT id FROM xt WHERE id = 3;' \
        'SELECT COUNT(*) FROM xt WHERE id < 5;'
)"
require_contains 'id
3' "$xtype_out"
require_contains 'agg_0
1' "$xtype_out"

# INSERT must coerce a numeric value to the column's declared type, so the
# stored type matches the schema regardless of the literal's spelling. An
# integer literal inserted into a DOUBLE column must read back as a double.
coerce_out="$(
    run_sql \
        'CREATE TABLE coercet (id INT PRIMARY KEY, d DOUBLE, f FLOAT);' \
        'INSERT INTO coercet VALUES (1, 5, 5);' \
        'SELECT d, f FROM coercet WHERE id = 1;'
)"
require_contains '5.000000 | 5.000000' "$coerce_out"

# UPDATE must coerce a SET value to the column's declared numeric type too.
ucoerce_out="$(
    run_sql \
        'CREATE TABLE ucoercet (id INT PRIMARY KEY, d DOUBLE);' \
        'INSERT INTO ucoercet VALUES (1, 1.5);' \
        'UPDATE ucoercet SET d = 7 WHERE id = 1;' \
        'SELECT d FROM ucoercet WHERE id = 1;'
)"
require_contains '7.000000' "$ucoerce_out"

# CAST of an out-of-range value to a narrower integer type must yield NULL,
# not a silently wrapped/garbage value (the fast numeric cast path used to
# wrap or invoke UB).
cast_out="$(
    run_sql \
        'SELECT CAST(5000000000 AS INT) AS a;' \
        'SELECT CAST(3000000000.0 AS INT) AS b;' \
        'SELECT CAST(2000000000 AS INT) AS c;'
)"
require_contains 'a
NULL' "$cast_out"
require_contains 'c
2000000000' "$cast_out"
require_not_contains '705032704' "$cast_out"

# Positional ORDER BY ("ORDER BY 2") must reference the Nth select item, not be
# treated as an ignored constant. Covers plain, aggregate, and multi-key forms.
pos_out="$(
    run_sql \
        'CREATE TABLE post (id INT PRIMARY KEY, a INT, b INT);' \
        'INSERT INTO post VALUES (1,10,1),(2,10,2),(3,20,1),(4,20,3),(5,20,5);' \
        'SELECT id FROM post ORDER BY 1 DESC;' \
        'SELECT a, COUNT(*) FROM post GROUP BY a ORDER BY 2 DESC;'
)"
require_contains 'id
5
4
3
2
1' "$pos_out"
require_contains 'a | agg_0
20 | 3
10 | 2' "$pos_out"
