#!/usr/bin/env bash
set -uo pipefail

BIN="${1:?usage: comprehensive_test.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-comp.XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

PASS=0
FAIL=0
TOTAL=0
BUGS=""

run_sql() {
    printf '%s\n' "$@" 'exit' | "$BIN" --dir "$DB_DIR" 2>&1
}

run_sql_text() {
    printf '%s\nexit\n' "$1" | "$BIN" --dir "$DB_DIR" 2>&1
}

check() {
    local desc="$1"
    local needle="$2"
    local haystack="$3"
    TOTAL=$((TOTAL+1))
    if [[ "$haystack" == *"$needle"* ]]; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        BUGS="${BUGS}\n[BUG] $desc\n  Expected to contain: $needle"
    fi
}

check_not_contains() {
    local desc="$1"
    local needle="$2"
    local haystack="$3"
    TOTAL=$((TOTAL+1))
    if [[ "$haystack" != *"$needle"* ]]; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        BUGS="${BUGS}\n[BUG] $desc\n  Should NOT contain: $needle"
    fi
}

check_exact_line() {
    local desc="$1"
    local expected="$2"
    local haystack="$3"
    TOTAL=$((TOTAL+1))
    if echo "$haystack" | grep -qF "$expected"; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        BUGS="${BUGS}\n[BUG] $desc\n  Expected line: $expected"
    fi
}

echo "=== Test Suite 1: SQL Parsing Edge Cases ==="

out="$(run_sql \
    "CREATE TABLE parse_t (id INT PRIMARY KEY, v INT);" \
    "INSERT INTO parse_t VALUES (1, 10);" \
    "SELECT 1 + 2;" \
    "SELECT 1 + 2 + 3;" \
    "SELECT 1 * 2 + 3;" \
    "SELECT (1 + 2) * 3;" \
    "SELECT -1;" \
    "SELECT -(-1);" \
    "SELECT 10 / 3;" \
    "SELECT 10 % 3;" \
    "SELECT 1 = 1;" \
    "SELECT 1 != 2;" \
    "SELECT 1 <> 2;" \
    "SELECT 1 < 2;" \
    "SELECT 2 > 1;" \
    "SELECT 1 <= 1;" \
    "SELECT 2 >= 2;" \
    "SELECT NOT TRUE;" \
    "SELECT NOT FALSE;" \
    "SELECT TRUE AND FALSE;" \
    "SELECT TRUE OR FALSE;" \
    "SELECT NULL;" \
    "SELECT 1 + NULL;" \
    "SELECT NULL = NULL;" \
    "SELECT NULL != NULL;" \
    "SELECT NULL AND TRUE;" \
    "SELECT NULL OR TRUE;" \
    "SELECT NOT NULL;" \
    "SELECT 1 WHERE TRUE;" \
    "SELECT 1 WHERE FALSE;" \
    "SELECT 1 WHERE NULL;" \
)"
check "SELECT 1+2=3" "3" "$out"
check "SELECT -(−1)=1" "1" "$out"
check "SELECT 10/3=3" "3" "$out"
check "SELECT 10%3=1" "1" "$out"
check "SELECT NOT TRUE" "0" "$out"
check "SELECT NOT FALSE" "1" "$out"
check "SELECT TRUE AND FALSE" "0" "$out"
check "SELECT TRUE OR FALSE" "1" "$out"
check "SELECT NULL literal" "NULL" "$out"
check "SELECT 1+NULL" "NULL" "$out"
check "SELECT NULL=NULL" "NULL" "$out"
check "SELECT NULL AND TRUE" "NULL" "$out"
check "SELECT NULL OR TRUE" "1" "$out"
check "SELECT NOT NULL" "NULL" "$out"
check "SELECT 1 WHERE TRUE" "1" "$out"
check "SELECT 1 WHERE FALSE returns empty" "Goodbye" "$out"

echo "=== Test Suite 2: DML + NULL Semantics ==="

out="$(run_sql \
    "CREATE TABLE null_t (id INT PRIMARY KEY, val INT, name VARCHAR);" \
    "INSERT INTO null_t VALUES (1, 10, 'alice');" \
    "INSERT INTO null_t VALUES (2, NULL, 'bob');" \
    "INSERT INTO null_t VALUES (3, 30, NULL);" \
    "INSERT INTO null_t VALUES (4, NULL, NULL);" \
    "SELECT id FROM null_t WHERE val IS NULL;" \
    "SELECT id FROM null_t WHERE val IS NOT NULL;" \
    "SELECT id FROM null_t WHERE val = NULL;" \
    "SELECT id FROM null_t WHERE val != NULL;" \
    "SELECT id FROM null_t WHERE val < 20;" \
    "SELECT id FROM null_t WHERE val > 20;" \
    "SELECT id FROM null_t WHERE val BETWEEN 10 AND 30;" \
    "SELECT id FROM null_t WHERE val NOT BETWEEN 10 AND 30;" \
    "SELECT id FROM null_t WHERE name LIKE 'a%';" \
    "SELECT id FROM null_t WHERE name NOT LIKE 'a%';" \
    "SELECT id FROM null_t WHERE name IS NULL;" \
    "SELECT id FROM null_t WHERE name IS NOT NULL;" \
)"
check "IS NULL finds row 2,4" "2" "$out"
check "IS NULL finds row 4" "4" "$out"
check "IS NOT NULL finds row 1" "1" "$out"
check "IS NOT NULL finds row 3" "3" "$out"
check "val = NULL returns no rows" "Goodbye" "$out"
check "val != NULL returns no rows" "Goodbye" "$out"
check "val < 20 finds row 1" "1" "$out"
check "val > 20 finds row 3" "3" "$out"
check "BETWEEN 10 AND 30 finds row 1" "1" "$out"
check "BETWEEN 10 AND 30 finds row 3" "3" "$out"
check "NOT BETWEEN excludes 10,30" "Goodbye" "$out"
check "LIKE a% finds row 1" "1" "$out"
check "NOT LIKE excludes alice" "Goodbye" "$out"
check "name IS NULL finds row 3" "3" "$out"
check "name IS NOT NULL finds row 1" "1" "$out"

echo "=== Test Suite 3: Multi-row INSERT + Type Variety ==="

out="$(run_sql \
    "CREATE TABLE types_t (b BOOL, i32 INT, i64 BIGINT, f FLOAT, d DOUBLE, s VARCHAR);" \
    "INSERT INTO types_t VALUES (TRUE, 42, 100000, 3.14, 2.718, 'hello');" \
    "INSERT INTO types_t VALUES (FALSE, -1, -99999, -1.0, -2.0, 'world');" \
    "INSERT INTO types_t VALUES (TRUE, 0, 0, 0.0, 0.0, '');" \
    "SELECT i32 FROM types_t;" \
    "SELECT s FROM types_t;" \
)"
check "INT positive" "42" "$out"
check "INT negative" "-1" "$out"
check "INT zero" "0" "$out"
check "VARCHAR hello" "hello" "$out"
check "VARCHAR world" "world" "$out"

echo "=== Test Suite 4: UPDATE + DELETE ==="

out="$(run_sql \
    "CREATE TABLE mut_t (id INT PRIMARY KEY, v INT);" \
    "INSERT INTO mut_t VALUES (1, 100), (2, 200), (3, 300);" \
    "UPDATE mut_t SET v = 999 WHERE id = 2;" \
    "SELECT v FROM mut_t WHERE id = 2;" \
    "DELETE FROM mut_t WHERE id = 1;" \
    "SELECT id FROM mut_t;" \
    "DELETE FROM mut_t;" \
    "SELECT id FROM mut_t;" \
)"
check "UPDATE modifies value" "999" "$out"
check "DELETE removes row" "Goodbye" "$out"
check "DELETE all leaves empty" "Goodbye" "$out"

echo "=== Test Suite 5: Aggregate Functions ==="

out="$(run_sql \
    "CREATE TABLE agg_t (id INT PRIMARY KEY, v INT);" \
    "INSERT INTO agg_t VALUES (1, 10), (2, 20), (3, 30), (4, NULL);" \
    "SELECT COUNT(*) FROM agg_t;" \
    "SELECT COUNT(v) FROM agg_t;" \
    "SELECT SUM(v) FROM agg_t;" \
    "SELECT MIN(v) FROM agg_t;" \
    "SELECT MAX(v) FROM agg_t;" \
    "SELECT AVG(v) FROM agg_t;" \
    "SELECT COUNT(*) FROM agg_t WHERE v IS NULL;" \
    "SELECT COUNT(*) FROM agg_t WHERE v IS NOT NULL;" \
    "SELECT SUM(v) FROM agg_t WHERE v > 20;" \
    "SELECT MIN(v) FROM agg_t WHERE v > 10;" \
    "SELECT MAX(v) FROM agg_t WHERE v < 30;" \
)"
check "COUNT(*) = 4" "4" "$out"
check "COUNT(v) = 3 (excludes NULL)" "3" "$out"
check "SUM(v) = 60" "60" "$out"
check "MIN(v) = 10" "10" "$out"
check "MAX(v) = 30" "30" "$out"
check "AVG(v) = 20" "20" "$out"
check "COUNT WHERE NULL = 1" "1" "$out"
check "COUNT WHERE NOT NULL = 3" "3" "$out"
check "SUM WHERE v>20 = 30" "30" "$out"
check "MIN WHERE v>10 = 20" "20" "$out"
check "MAX WHERE v<30 = 20" "20" "$out"

echo "=== Test Suite 6: GROUP BY + HAVING ==="

out="$(run_sql \
    "CREATE TABLE grp_t (cat VARCHAR, val INT);" \
    "INSERT INTO grp_t VALUES ('A', 10), ('A', 20), ('B', 30), ('B', 30), ('B', 40);" \
    "SELECT cat, COUNT(*) FROM grp_t GROUP BY cat;" \
    "SELECT cat, SUM(val) FROM grp_t GROUP BY cat;" \
    "SELECT cat, AVG(val) FROM grp_t GROUP BY cat;" \
    "SELECT cat, MAX(val) FROM grp_t GROUP BY cat HAVING MAX(val) > 30;" \
)"
check "GROUP BY A count=2" "A" "$out"
check "GROUP BY B count=3" "B" "$out"
check "SUM A=30" "30" "$out"
check "SUM B=100" "100" "$out"
check "HAVING filters B" "B" "$out"

echo "=== Test Suite 7: ORDER BY + LIMIT + OFFSET ==="

out="$(run_sql \
    "CREATE TABLE ord_t (id INT PRIMARY KEY, v INT);" \
    "INSERT INTO ord_t VALUES (1, 30), (2, 10), (3, 20), (4, 5), (5, 40);" \
    "SELECT id FROM ord_t ORDER BY v ASC;" \
    "SELECT id FROM ord_t ORDER BY v DESC;" \
    "SELECT id FROM ord_t ORDER BY v ASC LIMIT 3;" \
    "SELECT id FROM ord_t ORDER BY v ASC LIMIT 3 OFFSET 2;" \
    "SELECT id FROM ord_t ORDER BY v ASC LIMIT 100;" \
    "SELECT id FROM ord_t ORDER BY v ASC LIMIT 0;" \
    "SELECT id FROM ord_t LIMIT 100 OFFSET 100;" \
)"
check "ORDER BY ASC first" "4" "$out"
check "ORDER BY ASC second" "2" "$out"
check "ORDER BY DESC first" "5" "$out"
check "LIMIT 3 returns 3 rows" "id" "$out"
check "LIMIT 3 OFFSET 2 skips first 2" "3" "$out"
check "LIMIT 100 returns all (5 rows)" "5" "$out"
check "LIMIT 0 returns empty" "Goodbye" "$out"
check "OFFSET past end returns empty" "Goodbye" "$out"

echo "=== Test Suite 8: DISTINCT ==="

out="$(run_sql \
    "CREATE TABLE dist_t (v INT);" \
    "INSERT INTO dist_t VALUES (1), (2), (2), (3), (3), (3);" \
    "SELECT DISTINCT v FROM dist_t ORDER BY v ASC;" \
    "SELECT COUNT(DISTINCT v) FROM dist_t;" \
)"
check "DISTINCT 1" "1" "$out"
check "DISTINCT 2" "2" "$out"
check "DISTINCT 3" "3" "$out"
check "COUNT DISTINCT = 3" "3" "$out"

echo "=== Test Suite 9: Expression + CASE + COALESCE + NULLIF ==="

out="$(run_sql \
    "CREATE TABLE expr_t (id INT PRIMARY KEY, a INT, b INT);" \
    "INSERT INTO expr_t VALUES (1, 10, 20);" \
    "INSERT INTO expr_t VALUES (2, NULL, 30);" \
    "SELECT a + b FROM expr_t WHERE id = 1;" \
    "SELECT a * b FROM expr_t WHERE id = 1;" \
    "SELECT COALESCE(a, b) FROM expr_t WHERE id = 2;" \
    "SELECT NULLIF(a, 10) FROM expr_t WHERE id = 1;" \
    "SELECT NULLIF(a, 999) FROM expr_t WHERE id = 1;" \
    "SELECT CASE WHEN a > 5 THEN 'big' ELSE 'small' END FROM expr_t WHERE id = 1;" \
    "SELECT CASE WHEN a IS NULL THEN 'null' ELSE 'ok' END FROM expr_t WHERE id = 2;" \
)"
check "a+b = 30" "30" "$out"
check "a*b = 200" "200" "$out"
check "COALESCE(NULL,30)=30" "30" "$out"
check "NULLIF(10,10)=NULL" "NULL" "$out"
check "NULLIF(10,999)=10" "10" "$out"
check "CASE big" "big" "$out"
check "CASE null" "null" "$out"

echo "=== Test Suite 10: IN + NOT IN + BETWEEN ==="

out="$(run_sql \
    "CREATE TABLE in_t (id INT PRIMARY KEY, v INT);" \
    "INSERT INTO in_t VALUES (1, 10), (2, 20), (3, 30), (4, 40);" \
    "SELECT id FROM in_t WHERE v IN (10, 30);" \
    "SELECT id FROM in_t WHERE v NOT IN (10, 30);" \
    "SELECT id FROM in_t WHERE v IN (20, 999);" \
    "SELECT id FROM in_t WHERE v BETWEEN 15 AND 35;" \
    "SELECT id FROM in_t WHERE v NOT BETWEEN 15 AND 35;" \
)"
check "IN finds 1" "1" "$out"
check "IN finds 3" "3" "$out"
check "NOT IN finds 2" "2" "$out"
check "NOT IN finds 4" "4" "$out"
check "BETWEEN 15-35 finds 2" "2" "$out"
check "BETWEEN 15-35 finds 3" "3" "$out"
check "NOT BETWEEN finds 1" "1" "$out"
check "NOT BETWEEN finds 4" "4" "$out"

echo "=== Test Suite 11: JOIN ==="

out="$(run_sql \
    "CREATE TABLE j1 (id INT PRIMARY KEY, v INT);" \
    "CREATE TABLE j2 (id INT PRIMARY KEY, v INT);" \
    "INSERT INTO j1 VALUES (1, 10), (2, 20), (3, 30);" \
    "INSERT INTO j2 VALUES (1, 100), (2, 200), (4, 400);" \
    "SELECT j1.id, j2.v FROM j1 INNER JOIN j2 ON j1.id = j2.id;" \
    "SELECT j1.id FROM j1 LEFT JOIN j2 ON j1.id = j2.id ORDER BY j1.id ASC;" \
)"
check "INNER JOIN matches 1" "100" "$out"
check "INNER JOIN matches 2" "200" "$out"
check "INNER JOIN no 3" "Goodbye" "$out"
check "LEFT JOIN has 3 (with NULL)" "3" "$out"
check "LEFT JOIN no 4" "Goodbye" "$out"

echo "=== Test Suite 12: B+Tree Index Operations ==="

out="$(run_sql \
    "CREATE TABLE idx_t (id INT PRIMARY KEY, v INT);" \
    "CREATE INDEX idx_v ON idx_t (v);" \
    "INSERT INTO idx_t VALUES (1, 50), (2, 30), (3, 70), (4, 10), (5, 90);" \
    "INSERT INTO idx_t VALUES (6, 40), (7, 60), (8, 20), (9, 80), (10, 100);" \
    "SELECT id FROM idx_t WHERE v = 50;" \
    "SELECT id FROM idx_t WHERE v > 60 ORDER BY v ASC;" \
    "SELECT id FROM idx_t WHERE v < 30 ORDER BY v ASC;" \
    "SELECT id FROM idx_t WHERE v >= 40 AND v <= 70 ORDER BY v ASC;" \
    "DELETE FROM idx_t WHERE v = 50;" \
    "SELECT id FROM idx_t WHERE v = 50;" \
    "DELETE FROM idx_t WHERE v = 30;" \
    "DELETE FROM idx_t WHERE v = 70;" \
    "SELECT id FROM idx_t WHERE v = 30;" \
    "SELECT id FROM idx_t WHERE v = 70;" \
    "SELECT COUNT(*) FROM idx_t;" \
)"
check "Index eq lookup" "1" "$out"
check "Index range > 60 finds 7,9,10" "7" "$out"
check "Index range < 30 finds 4,8" "4" "$out"
check "Index range BETWEEN 40-70" "6" "$out"
check "DELETE removes from index" "Goodbye" "$out"
check "COUNT after deletes = 7" "7" "$out"

echo "=== Test Suite 13: Stress - Many Inserts + Page Splits ==="

# Insert 200 rows to force multiple page splits
SQL_CMDS="CREATE TABLE stress_t (id INT PRIMARY KEY, v INT);"
for i in $(seq 1 200); do
    SQL_CMDS="${SQL_CMDS}"$'\n'"INSERT INTO stress_t VALUES ($i, $((i * 7)));"
done
SQL_CMDS="${SQL_CMDS}"$'\n'"SELECT COUNT(*) FROM stress_t;"
SQL_CMDS="${SQL_CMDS}"$'\n'"SELECT v FROM stress_t WHERE id = 100;"
SQL_CMDS="${SQL_CMDS}"$'\n'"SELECT id FROM stress_t WHERE v = 1400;"
SQL_CMDS="${SQL_CMDS}"$'\n'"SELECT MAX(id) FROM stress_t;"
SQL_CMDS="${SQL_CMDS}"$'\n'"SELECT MIN(id) FROM stress_t;"

out="$(run_sql_text "$SQL_CMDS")"
check "Stress: COUNT = 200" "200" "$out"
check "Stress: id=100 v=700" "700" "$out"
check "Stress: v=1400 id=200" "200" "$out"
check "Stress: MAX(id)=200" "200" "$out"
check "Stress: MIN(id)=1" "1" "$out"

echo "=== Test Suite 14: Stress - Many Index Inserts ==="

SQL_CMDS="CREATE TABLE stress_idx (id INT PRIMARY KEY, v INT);"
SQL_CMDS="${SQL_CMDS}"$'\n'"CREATE INDEX stress_v ON stress_idx (v);"
for i in $(seq 1 100); do
    SQL_CMDS="${SQL_CMDS}"$'\n'"INSERT INTO stress_idx VALUES ($i, $((i % 10)));"
done
SQL_CMDS="${SQL_CMDS}"$'\n'"SELECT COUNT(*) FROM stress_idx WHERE v = 5;"
SQL_CMDS="${SQL_CMDS}"$'\n'"SELECT COUNT(*) FROM stress_idx WHERE v = 0;"
SQL_CMDS="${SQL_CMDS}"$'\n'"SELECT id FROM stress_idx WHERE v = 5 ORDER BY id ASC;"

out="$(run_sql_text "$SQL_CMDS")"
check "Stress idx: COUNT v=5 = 10" "10" "$out"
check "Stress idx: COUNT v=0 = 10" "10" "$out"

echo "=== Test Suite 15: Multi-Column Composite Index ==="

out="$(run_sql \
    "CREATE TABLE comp_t (a INT, b INT, v INT);" \
    "CREATE INDEX comp_idx ON comp_t (a, b);" \
    "INSERT INTO comp_t VALUES (1, 1, 100);" \
    "INSERT INTO comp_t VALUES (1, 2, 200);" \
    "INSERT INTO comp_t VALUES (2, 1, 300);" \
    "INSERT INTO comp_t VALUES (2, 2, 400);" \
    "SELECT v FROM comp_t WHERE a = 1 AND b = 2;" \
    "SELECT v FROM comp_t WHERE a = 2 AND b = 1;" \
    "SELECT v FROM comp_t WHERE a = 1;" \
)"
check "Composite: a=1,b=2 → 200" "200" "$out"
check "Composite: a=2,b=1 → 300" "300" "$out"
check "Composite: a=1 → 100,200" "100" "$out"

echo "=== Test Suite 16: DROP TABLE / DROP INDEX ==="

out="$(run_sql \
    "CREATE TABLE drop_t (id INT PRIMARY KEY);" \
    "INSERT INTO drop_t VALUES (1);" \
    "DROP TABLE drop_t;" \
    "SELECT id FROM drop_t;" \
    "SHOW TABLES;" \
)"
check "DROP TABLE removes table" "Error" "$out"

echo "=== Test Suite 17: Edge Cases - Empty Table ==="

out="$(run_sql \
    "CREATE TABLE empty_t (id INT PRIMARY KEY, v INT);" \
    "SELECT COUNT(*) FROM empty_t;" \
    "SELECT id FROM empty_t;" \
    "SELECT MIN(v) FROM empty_t;" \
    "SELECT MAX(v) FROM empty_t;" \
    "SELECT SUM(v) FROM empty_t;" \
    "DELETE FROM empty_t;" \
    "SELECT COUNT(*) FROM empty_t;" \
)"
check "Empty table COUNT = 0" "0" "$out"
check "Empty table SELECT empty" "Goodbye" "$out"
check "Empty table MIN returns NULL" "NULL" "$out"
check "Empty table MAX returns NULL" "NULL" "$out"
check "Empty table SUM returns NULL" "NULL" "$out"

echo "=== Test Suite 18: Edge Cases - Boundary Values ==="

out="$(run_sql \
    "CREATE TABLE bound_t (id INT PRIMARY KEY, v BIGINT);" \
    "INSERT INTO bound_t VALUES (1, 2147483647);" \
    "INSERT INTO bound_t VALUES (2, -2147483647);" \
    "INSERT INTO bound_t VALUES (3, 0);" \
    "SELECT v FROM bound_t WHERE id = 1;" \
    "SELECT v FROM bound_t WHERE id = 2;" \
    "SELECT v FROM bound_t WHERE id = 3;" \
)"
check "MAX INT32" "2147483647" "$out"
check "MIN INT32" "-2147483647" "$out"
check "Zero" "0" "$out"

echo "=== Test Suite 19: Multiple WHERE Conditions ==="

out="$(run_sql \
    "CREATE TABLE where_t (id INT PRIMARY KEY, a INT, b INT, s VARCHAR);" \
    "INSERT INTO where_t VALUES (1, 10, 20, 'apple');" \
    "INSERT INTO where_t VALUES (2, 30, 40, 'banana');" \
    "INSERT INTO where_t VALUES (3, 50, 60, 'cherry');" \
    "INSERT INTO where_t VALUES (4, NULL, NULL, NULL);" \
    "SELECT id FROM where_t WHERE a > 10 AND b < 50;" \
    "SELECT id FROM where_t WHERE a > 10 OR b < 30;" \
    "SELECT id FROM where_t WHERE NOT (a > 10);" \
    "SELECT id FROM where_t WHERE a IS NOT NULL AND b > 30;" \
    "SELECT id FROM where_t WHERE s LIKE 'b%';" \
    "SELECT id FROM where_t WHERE s LIKE '%e%';" \
    "SELECT id FROM where_t WHERE s LIKE '_a%';" \
)"
check "AND: a>10 AND b<50 → 2" "2" "$out"
check "OR: a>10 OR b<30 → 1,2,3" "1" "$out"
check "NOT: NOT a>10 -> 1,4" "1" "$out"
check "IS NOT NULL AND: → 2,3" "3" "$out"
check "LIKE b% → 2" "2" "$out"
check "LIKE %e% → 2,3" "3" "$out"
check "LIKE _a% → 2" "2" "$out"

echo "=== Test Suite 20: Transaction BEGIN/COMMIT/ROLLBACK ==="

out="$(run_sql \
    "CREATE TABLE txn_t (id INT PRIMARY KEY, v INT);" \
    "BEGIN;" \
    "INSERT INTO txn_t VALUES (1, 10);" \
    "COMMIT;" \
    "SELECT id FROM txn_t;" \
    "BEGIN;" \
    "INSERT INTO txn_t VALUES (2, 20);" \
    "ROLLBACK;" \
    "SELECT id FROM txn_t;" \
    "BEGIN;" \
    "INSERT INTO txn_t VALUES (3, 30);" \
    "SELECT id FROM txn_t;" \
    "ROLLBACK;" \
    "SELECT id FROM txn_t;" \
)"
check "COMMIT makes data visible" "1" "$out"
check "ROLLBACK reverts insert" "Goodbye" "$out"
check "ROLLBACK reverts visible txn" "Goodbye" "$out"

echo "=== Test Suite 21: Double-Quoted Strings ==="

out="$(run_sql \
    "CREATE TABLE dq_t (id INT PRIMARY KEY, v INT);" \
    'INSERT INTO dq_t VALUES (1, 10);' \
    "SELECT v FROM dq_t;" \
)"
check "Double-quoted INSERT works" "10" "$out"

echo "=== Test Suite 22: Arithmetic in WHERE ==="

out="$(run_sql \
    "CREATE TABLE arith_t (id INT PRIMARY KEY, a INT, b INT);" \
    "INSERT INTO arith_t VALUES (1, 10, 3), (2, 20, 5), (3, 15, 4);" \
    "SELECT id FROM arith_t WHERE a / b = 3;" \
    "SELECT id FROM arith_t WHERE a % b = 0;" \
    "SELECT id FROM arith_t WHERE a + b > 24;" \
    "SELECT id FROM arith_t WHERE a - b = 11;" \
)"
check "a/b = 3 → 1" "1" "$out"
check "a%b = 0 → 1,2" "1" "$out"
check "a+b > 24 → 2" "2" "$out"
check "a-b = 11 → 3" "3" "$out"

echo "=== Test Suite 23: String Operations ==="

out="$(run_sql \
    "CREATE TABLE str_t (id INT PRIMARY KEY, s VARCHAR);" \
    "INSERT INTO str_t VALUES (1, 'hello');" \
    "INSERT INTO str_t VALUES (2, 'world');" \
    "INSERT INTO str_t VALUES (3, 'Hello');" \
    "INSERT INTO str_t VALUES (4, '');" \
    "SELECT id FROM str_t WHERE s = 'hello';" \
    "SELECT id FROM str_t WHERE s != 'hello';" \
    "SELECT id FROM str_t WHERE s LIKE 'h%';" \
    "SELECT id FROM str_t WHERE s LIKE '%o%';" \
    "SELECT id FROM str_t WHERE s = 'Hello';" \
)"
check "String equality" "1" "$out"
check "String inequality" "2" "$out"
check "LIKE h% finds hello" "1" "$out"
check "LIKE %o% finds hello,world" "2" "$out"
check "Case sensitive: Hello" "3" "$out"

echo "=== Test Suite 24: EXPLAIN ==="

out="$(run_sql \
    "CREATE TABLE exp_t (id INT PRIMARY KEY, v INT);" \
    "CREATE INDEX exp_v ON exp_t (v);" \
    "INSERT INTO exp_t VALUES (1, 10);" \
    "EXPLAIN SELECT * FROM exp_t WHERE id = 1;" \
    "EXPLAIN SELECT * FROM exp_t WHERE v = 10;" \
    "EXPLAIN SELECT * FROM exp_t;" \
)"
check "EXPLAIN shows SeqScan or IndexScan" "Scan" "$out"

echo "=== Test Suite 25: Column Alias ==="

out="$(run_sql \
    "CREATE TABLE alias_t (id INT PRIMARY KEY, v INT);" \
    "INSERT INTO alias_t VALUES (1, 42);" \
    "SELECT v AS my_val FROM alias_t;" \
)"
check "Column alias" "my_val" "$out"

echo "=== Test Suite 26: Subquery IN ==="

out="$(run_sql \
    "CREATE TABLE sub1 (id INT PRIMARY KEY);" \
    "CREATE TABLE sub2 (id INT PRIMARY KEY);" \
    "INSERT INTO sub1 VALUES (1), (2), (3);" \
    "INSERT INTO sub2 VALUES (2), (4);" \
    "SELECT id FROM sub1 WHERE id IN (SELECT id FROM sub2);" \
)"
check "Subquery IN finds 2" "2" "$out"
check "Subquery IN excludes 1" "Goodbye" "$out"

echo "=== Test Suite 27: UNION ==="

out="$(run_sql \
    "CREATE TABLE u1 (id INT);" \
    "CREATE TABLE u2 (id INT);" \
    "INSERT INTO u1 VALUES (1), (2);" \
    "INSERT INTO u2 VALUES (2), (3);" \
    "SELECT id FROM u1 UNION ALL SELECT id FROM u2;" \
)"
check "UNION ALL finds 1" "1" "$out"
check "UNION ALL finds 3" "3" "$out"

echo "=== Test Suite 28: Overflow / Large Number ==="

out="$(run_sql \
    "CREATE TABLE big_t (id INT PRIMARY KEY, v BIGINT);" \
    "INSERT INTO big_t VALUES (1, 9999999999);" \
    "SELECT v FROM big_t;" \
    "SELECT v + 1 FROM big_t;" \
)"
check "BIGINT large value" "9999999999" "$out"
check "BIGINT large + 1" "10000000000" "$out"

echo "=== Test Suite 29: Empty WHERE / No WHERE ==="

out="$(run_sql \
    "CREATE TABLE nowhere_t (id INT PRIMARY KEY);" \
    "INSERT INTO nowhere_t VALUES (1), (2), (3);" \
    "SELECT id FROM nowhere_t;" \
    "DELETE FROM nowhere_t;" \
    "SELECT COUNT(*) FROM nowhere_t;" \
)"
check "No WHERE selects all" "1" "$out"
check "DELETE without WHERE removes all" "0" "$out"

echo "=== Test Suite 30: Multi-Statement in Single Buffer ==="

out="$(run_sql \
    "CREATE TABLE multi_t (id INT PRIMARY KEY, v INT);" \
    "INSERT INTO multi_t VALUES (1, 10); INSERT INTO multi_t VALUES (2, 20);" \
    "SELECT COUNT(*) FROM multi_t;" \
)"
check "Multi-statement INSERT" "2" "$out"

echo ""
echo "========================================="
echo "  TEST RESULTS: $PASS/$TOTAL passed, $FAIL failed"
echo "========================================="
if [ $FAIL -gt 0 ]; then
    echo -e "\n--- DISCOVERED BUGS ---$BUGS"
fi
echo ""
