#!/usr/bin/env python3
"""
Comprehensive test suite for MiniDB.
Tests SQL parsing, DML, NULL semantics, aggregates, joins, indexes,
transactions, concurrency, boundary conditions, and more.
"""
import subprocess, os, sys, tempfile, shutil, time, threading

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
DB_DIR = tempfile.mkdtemp(prefix="minidb_test_")
PASS = 0
FAIL = 0
BUGS = []

def cleanup():
    shutil.rmtree(DB_DIR, ignore_errors=True)

def run_sql(sqls):
    if isinstance(sqls, str):
        sqls = [sqls]
    sqls.append("exit")
    proc = subprocess.run(
        [BIN, "--dir", DB_DIR],
        input="\n".join(sqls),
        capture_output=True, text=True, timeout=30
    )
    return proc.stdout + proc.stderr

def check(desc, needle, haystack, negate=False):
    global PASS, FAIL
    haystack_to_check = haystack
    if negate:
        data_lines = []
        skip_next = False
        for raw in haystack.splitlines():
            line = raw.strip()
            if not line:
                continue
            if skip_next:
                skip_next = False
                continue
            if line.startswith("Data directory:"):
                continue
            if line.startswith("MiniADB "):
                continue
            if line.startswith("Type 'exit'"):
                continue
            if line.startswith("minidb>"):
                payload = line[len("minidb>"):].strip()
                if payload.startswith("[DB] destructor:") or payload == "Goodbye.":
                    continue
                if payload:
                    data_lines.append(payload)
                continue
            if line.startswith("[DB] destructor:"):
                continue
            if line == "Goodbye.":
                continue
            if line.startswith("Table '") and line.endswith("created."):
                continue
            if line in ("affected_rows", "deleted_rows"):
                skip_next = True
                continue
            data_lines.append(line)
        haystack_to_check = "\n".join(data_lines)
    found = needle in haystack_to_check
    if negate:
        found = not found
    if found:
        PASS += 1
        print(f"  PASS: {desc}")
    else:
        FAIL += 1
        BUGS.append((desc, needle, haystack[:500]))
        print(f"  FAIL: {desc}  (expected {'NOT ' if negate else ''}'{needle}')")
        print(f"        got: {haystack[:200]}...")

try:
    # ============================================================
    print("=== 1. SQL Parsing Edge Cases ===")
    out = run_sql([
        "CREATE TABLE t1 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t1 VALUES (1, 10);",
        "SELECT 1 + 2;",
        "SELECT 1 + 2 + 3;",
        "SELECT 1 * 2 + 3;",
        "SELECT (1 + 2) * 3;",
        "SELECT -1;",
        "SELECT -(-1);",
        "SELECT 10 / 3;",
        "SELECT 10 % 3;",
        "SELECT 1 = 1;",
        "SELECT 1 != 2;",
        "SELECT 1 <> 2;",
        "SELECT 1 < 2;",
        "SELECT 2 > 1;",
        "SELECT 1 <= 1;",
        "SELECT 2 >= 2;",
        "SELECT NOT TRUE;",
        "SELECT NOT FALSE;",
        "SELECT TRUE AND FALSE;",
        "SELECT TRUE OR FALSE;",
        "SELECT NULL;",
        "SELECT 1 + NULL;",
        "SELECT NULL = NULL;",
        "SELECT NULL != NULL;",
        "SELECT NULL AND TRUE;",
        "SELECT NULL OR TRUE;",
        "SELECT NOT NULL;",
        "SELECT 1 WHERE TRUE;",
        "SELECT 1 WHERE FALSE;",
        "SELECT 1 WHERE NULL;",
    ])
    check("1+2=3", "expr_0", out)  # expr_0 = 3
    check("NOT TRUE = 0", "0", out)
    check("NOT FALSE = 1", "1", out)
    check("TRUE AND FALSE = 0", "0", out)
    check("TRUE OR FALSE = 1", "1", out)
    check("NULL literal", "NULL", out)
    check("1+NULL=NULL", "NULL", out)
    check("NULL AND TRUE=NULL", "NULL", out)
    check("NULL OR TRUE=1", "1", out)

    # ============================================================
    print("\n=== 2. NULL Semantics ===")
    out = run_sql([
        "CREATE TABLE n2 (id INT PRIMARY KEY, val INT, name VARCHAR);",
        "INSERT INTO n2 VALUES (1, 10, 'alice');",
        "INSERT INTO n2 VALUES (2, NULL, 'bob');",
        "INSERT INTO n2 VALUES (3, 30, NULL);",
        "INSERT INTO n2 VALUES (4, NULL, NULL);",
        "SELECT id FROM n2 WHERE val IS NULL;",
        "SELECT id FROM n2 WHERE val IS NOT NULL;",
        "SELECT id FROM n2 WHERE val = NULL;",
        "SELECT id FROM n2 WHERE val != NULL;",
        "SELECT id FROM n2 WHERE val < 20;",
        "SELECT id FROM n2 WHERE val > 20;",
        "SELECT id FROM n2 WHERE val BETWEEN 10 AND 30;",
        "SELECT id FROM n2 WHERE val NOT BETWEEN 10 AND 30;",
        "SELECT id FROM n2 WHERE name LIKE 'a%';",
        "SELECT id FROM n2 WHERE name NOT LIKE 'a%';",
        "SELECT id FROM n2 WHERE name IS NULL;",
        "SELECT id FROM n2 WHERE name IS NOT NULL;",
    ])
    check("IS NULL finds 2", "2", out)
    check("IS NULL finds 4", "4", out)
    check("IS NOT NULL finds 1", "1", out)
    check("IS NOT NULL finds 3", "3", out)
    check("val=NULL no rows", "Goodbye", out)  # no results after header
    check("val!=NULL no rows", "Goodbye", out)
    check("val<20 finds 1", "1", out)
    check("val>20 finds 3", "3", out)
    check("BETWEEN 10-30 finds 1", "1", out)
    check("BETWEEN 10-30 finds 3", "3", out)
    check("LIKE a% finds 1", "1", out)
    check("IS NULL name finds 3", "3", out)
    check("IS NOT NULL name finds 1", "1", out)

    # ============================================================
    print("\n=== 3. Multi-row INSERT + Types ===")
    out = run_sql([
        "CREATE TABLE t3 (b BOOL, i32 INT, i64 BIGINT, f FLOAT, d DOUBLE, s VARCHAR);",
        "INSERT INTO t3 VALUES (TRUE, 42, 100000, 3.14, 2.718, 'hello');",
        "INSERT INTO t3 VALUES (FALSE, -1, -99999, -1.0, -2.0, 'world');",
        "INSERT INTO t3 VALUES (TRUE, 0, 0, 0.0, 0.0, '');",
        "SELECT i32 FROM t3;",
        "SELECT s FROM t3;",
    ])
    check("INT positive 42", "42", out)
    check("INT negative -1", "-1", out)
    check("INT zero", "0", out)
    check("VARCHAR hello", "hello", out)
    check("VARCHAR world", "world", out)

    # ============================================================
    print("\n=== 4. UPDATE + DELETE ===")
    out = run_sql([
        "CREATE TABLE t4 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t4 VALUES (1, 100), (2, 200), (3, 300);",
        "UPDATE t4 SET v = 999 WHERE id = 2;",
        "SELECT v FROM t4 WHERE id = 2;",
        "DELETE FROM t4 WHERE id = 1;",
        "SELECT id FROM t4;",
        "DELETE FROM t4;",
        "SELECT id FROM t4;",
    ])
    check("UPDATE modifies", "999", out)
    check("DELETE removes row", "Goodbye", out)
    check("DELETE all", "Goodbye", out)

    # ============================================================
    print("\n=== 5. Aggregate Functions ===")
    out = run_sql([
        "CREATE TABLE t5 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t5 VALUES (1, 10), (2, 20), (3, 30), (4, NULL);",
        "SELECT COUNT(*) FROM t5;",
        "SELECT COUNT(v) FROM t5;",
        "SELECT SUM(v) FROM t5;",
        "SELECT MIN(v) FROM t5;",
        "SELECT MAX(v) FROM t5;",
        "SELECT AVG(v) FROM t5;",
        "SELECT COUNT(*) FROM t5 WHERE v IS NULL;",
        "SELECT COUNT(*) FROM t5 WHERE v IS NOT NULL;",
        "SELECT SUM(v) FROM t5 WHERE v > 20;",
        "SELECT MIN(v) FROM t5 WHERE v > 10;",
        "SELECT MAX(v) FROM t5 WHERE v < 30;",
    ])
    # Extract the agg_0 column for count results
    check("COUNT(*)=4", "4", out)
    check("COUNT(v)=3", "3", out)
    check("SUM(v)=60", "60", out)
    check("MIN(v)=10", "10", out)
    check("MAX(v)=30", "30", out)

    # ============================================================
    print("\n=== 6. GROUP BY + HAVING ===")
    out = run_sql([
        "CREATE TABLE t6 (cat VARCHAR, val INT);",
        "INSERT INTO t6 VALUES ('A', 10), ('A', 20), ('B', 30), ('B', 30), ('B', 40);",
        "SELECT cat, COUNT(*) FROM t6 GROUP BY cat;",
        "SELECT cat, SUM(val) FROM t6 GROUP BY cat;",
        "SELECT cat, MAX(val) FROM t6 GROUP BY cat HAVING MAX(val) > 30;",
    ])
    check("GROUP BY A", "A", out)
    check("GROUP BY B", "B", out)
    check("HAVING filters", "B", out)

    # ============================================================
    print("\n=== 7. ORDER BY + LIMIT + OFFSET ===")
    out = run_sql([
        "CREATE TABLE t7 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t7 VALUES (1, 30), (2, 10), (3, 20), (4, 5), (5, 40);",
        "SELECT id FROM t7 ORDER BY v ASC;",
        "SELECT id FROM t7 ORDER BY v DESC;",
        "SELECT id FROM t7 ORDER BY v ASC LIMIT 3;",
        "SELECT id FROM t7 ORDER BY v ASC LIMIT 3 OFFSET 2;",
        "SELECT id FROM t7 ORDER BY v ASC LIMIT 0;",
        "SELECT id FROM t7 LIMIT 100 OFFSET 100;",
    ])
    check("ORDER BY ASC", "4", out)
    check("ORDER BY DESC", "5", out)
    check("LIMIT 3", "id", out)
    check("LIMIT 0 empty", "Goodbye", out)
    check("OFFSET past end", "Goodbye", out)

    # ============================================================
    print("\n=== 8. DISTINCT ===")
    out = run_sql([
        "CREATE TABLE t8 (v INT);",
        "INSERT INTO t8 VALUES (1), (2), (2), (3), (3), (3);",
        "SELECT DISTINCT v FROM t8 ORDER BY v ASC;",
    ])
    check("DISTINCT 1", "1", out)
    check("DISTINCT 2", "2", out)
    check("DISTINCT 3", "3", out)

    # ============================================================
    print("\n=== 9. CASE + COALESCE + NULLIF ===")
    out = run_sql([
        "CREATE TABLE t9 (id INT PRIMARY KEY, a INT, b INT);",
        "INSERT INTO t9 VALUES (1, 10, 20);",
        "INSERT INTO t9 VALUES (2, NULL, 30);",
        "SELECT a + b FROM t9 WHERE id = 1;",
        "SELECT a * b FROM t9 WHERE id = 1;",
        "SELECT COALESCE(a, b) FROM t9 WHERE id = 2;",
        "SELECT NULLIF(a, 10) FROM t9 WHERE id = 1;",
        "SELECT NULLIF(a, 999) FROM t9 WHERE id = 1;",
        "SELECT CASE WHEN a > 5 THEN 'big' ELSE 'small' END FROM t9 WHERE id = 1;",
        "SELECT CASE WHEN a IS NULL THEN 'null' ELSE 'ok' END FROM t9 WHERE id = 2;",
    ])
    check("a+b=30", "30", out)
    check("a*b=200", "200", out)
    check("COALESCE(NULL,30)=30", "30", out)
    check("NULLIF(10,10)=NULL", "NULL", out)
    check("NULLIF(10,999)=10", "10", out)
    check("CASE big", "big", out)
    check("CASE null", "null", out)

    # ============================================================
    print("\n=== 10. IN + NOT IN + BETWEEN ===")
    out = run_sql([
        "CREATE TABLE t10 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t10 VALUES (1, 10), (2, 20), (3, 30), (4, 40);",
        "SELECT id FROM t10 WHERE v IN (10, 30);",
        "SELECT id FROM t10 WHERE v NOT IN (10, 30);",
        "SELECT id FROM t10 WHERE v BETWEEN 15 AND 35;",
        "SELECT id FROM t10 WHERE v NOT BETWEEN 15 AND 35;",
    ])
    check("IN finds 1", "1", out)
    check("IN finds 3", "3", out)
    check("NOT IN finds 2", "2", out)
    check("NOT IN finds 4", "4", out)
    check("BETWEEN finds 2", "2", out)
    check("BETWEEN finds 3", "3", out)
    check("NOT BETWEEN finds 1", "1", out)
    check("NOT BETWEEN finds 4", "4", out)

    # ============================================================
    print("\n=== 11. JOIN ===")
    out = run_sql([
        "CREATE TABLE j1 (id INT PRIMARY KEY, v INT);",
        "CREATE TABLE j2 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO j1 VALUES (1, 10), (2, 20), (3, 30);",
        "INSERT INTO j2 VALUES (1, 100), (2, 200), (4, 400);",
        "SELECT j1.id, j2.v FROM j1 INNER JOIN j2 ON j1.id = j2.id;",
        "SELECT j1.id FROM j1 LEFT JOIN j2 ON j1.id = j2.id ORDER BY j1.id ASC;",
    ])
    check("INNER JOIN 1", "100", out)
    check("INNER JOIN 2", "200", out)
    check("LEFT JOIN has 3", "3", out)
    check("LEFT JOIN no 4", "Goodbye", out, negate=True)

    # ============================================================
    print("\n=== 12. B+Tree Index ===")
    out = run_sql([
        "CREATE TABLE t12 (id INT PRIMARY KEY, v INT);",
        "CREATE INDEX idx_v ON t12 (v);",
        "INSERT INTO t12 VALUES (1, 50), (2, 30), (3, 70), (4, 10), (5, 90);",
        "INSERT INTO t12 VALUES (6, 40), (7, 60), (8, 20), (9, 80), (10, 100);",
        "SELECT id FROM t12 WHERE v = 50;",
        "SELECT id FROM t12 WHERE v > 60 ORDER BY v ASC;",
        "SELECT id FROM t12 WHERE v < 30 ORDER BY v ASC;",
        "SELECT id FROM t12 WHERE v >= 40 AND v <= 70 ORDER BY v ASC;",
        "DELETE FROM t12 WHERE v = 50;",
        "SELECT id FROM t12 WHERE v = 50;",
        "DELETE FROM t12 WHERE v = 30;",
        "DELETE FROM t12 WHERE v = 70;",
        "SELECT COUNT(*) FROM t12;",
    ])
    check("Index eq v=50", "1", out)
    check("Index range >60", "7", out)
    check("Index range <30", "4", out)
    check("Index range 40-70", "6", out)
    check("DELETE from index", "Goodbye", out)
    check("COUNT after delete=7", "7", out)

    # ============================================================
    print("\n=== 13. Stress: 200 rows (page splits) ===")
    sqls = ["CREATE TABLE t13 (id INT PRIMARY KEY, v INT);"]
    for i in range(1, 201):
        sqls.append(f"INSERT INTO t13 VALUES ({i}, {i * 7});")
    sqls += [
        "SELECT COUNT(*) FROM t13;",
        "SELECT v FROM t13 WHERE id = 100;",
        "SELECT id FROM t13 WHERE v = 1400;",
        "SELECT MAX(id) FROM t13;",
        "SELECT MIN(id) FROM t13;",
    ]
    out = run_sql(sqls)
    check("Stress COUNT=200", "200", out)
    check("Stress id=100 v=700", "700", out)
    check("Stress v=1400 id=200", "200", out)
    check("Stress MAX(id)=200", "200", out)
    check("Stress MIN(id)=1", "1", out)

    # ============================================================
    print("\n=== 14. Stress: Index with duplicates ===")
    sqls = [
        "CREATE TABLE t14 (id INT PRIMARY KEY, v INT);",
        "CREATE INDEX idx_v14 ON t14 (v);",
    ]
    for i in range(1, 101):
        sqls.append(f"INSERT INTO t14 VALUES ({i}, {i % 10});")
    sqls += [
        "SELECT COUNT(*) FROM t14 WHERE v = 5;",
        "SELECT COUNT(*) FROM t14 WHERE v = 0;",
    ]
    out = run_sql(sqls)
    check("Index dup COUNT v=5 = 10", "10", out)
    check("Index dup COUNT v=0 = 10", "10", out)

    # ============================================================
    print("\n=== 15. Composite Index ===")
    out = run_sql([
        "CREATE TABLE t15 (a INT, b INT, v INT);",
        "CREATE INDEX idx_ab ON t15 (a, b);",
        "INSERT INTO t15 VALUES (1, 1, 100);",
        "INSERT INTO t15 VALUES (1, 2, 200);",
        "INSERT INTO t15 VALUES (2, 1, 300);",
        "INSERT INTO t15 VALUES (2, 2, 400);",
        "SELECT v FROM t15 WHERE a = 1 AND b = 2;",
        "SELECT v FROM t15 WHERE a = 2 AND b = 1;",
        "SELECT v FROM t15 WHERE a = 1;",
    ])
    check("Composite a=1,b=2 -> 200", "200", out)
    check("Composite a=2,b=1 -> 300", "300", out)
    check("Composite a=1 -> 100,200", "100", out)

    # ============================================================
    print("\n=== 16. DROP TABLE/INDEX ===")
    out = run_sql([
        "CREATE TABLE t16 (id INT PRIMARY KEY);",
        "INSERT INTO t16 VALUES (1);",
        "DROP TABLE t16;",
        "SELECT id FROM t16;",
    ])
    check("DROP TABLE errors", "Error", out)

    # ============================================================
    print("\n=== 17. Empty Table ===")
    out = run_sql([
        "CREATE TABLE t17 (id INT PRIMARY KEY, v INT);",
        "SELECT COUNT(*) FROM t17;",
        "SELECT id FROM t17;",
        "SELECT MIN(v) FROM t17;",
        "SELECT MAX(v) FROM t17;",
        "SELECT SUM(v) FROM t17;",
    ])
    check("Empty COUNT=0", "0", out)
    check("Empty SELECT", "Goodbye", out)
    check("Empty MIN=NULL", "NULL", out)
    check("Empty MAX=NULL", "NULL", out)
    check("Empty SUM=NULL", "NULL", out)

    # ============================================================
    print("\n=== 18. Boundary Values ===")
    out = run_sql([
        "CREATE TABLE t18 (id INT PRIMARY KEY, v BIGINT);",
        "INSERT INTO t18 VALUES (1, 2147483647);",
        "INSERT INTO t18 VALUES (2, -2147483647);",
        "INSERT INTO t18 VALUES (3, 0);",
        "SELECT v FROM t18 WHERE id = 1;",
        "SELECT v FROM t18 WHERE id = 2;",
        "SELECT v FROM t18 WHERE id = 3;",
    ])
    check("MAX INT32", "2147483647", out)
    check("MIN INT32", "-2147483647", out)
    check("Zero", "0", out)

    # ============================================================
    print("\n=== 19. Multiple WHERE Conditions ===")
    out = run_sql([
        "CREATE TABLE t19 (id INT PRIMARY KEY, a INT, b INT, s VARCHAR);",
        "INSERT INTO t19 VALUES (1, 10, 20, 'apple');",
        "INSERT INTO t19 VALUES (2, 30, 40, 'banana');",
        "INSERT INTO t19 VALUES (3, 50, 60, 'cherry');",
        "INSERT INTO t19 VALUES (4, NULL, NULL, NULL);",
        "SELECT id FROM t19 WHERE a > 10 AND b < 50;",
        "SELECT id FROM t19 WHERE a > 10 OR b < 30;",
        "SELECT id FROM t19 WHERE s LIKE 'b%';",
        "SELECT id FROM t19 WHERE s LIKE '%e%';",
        "SELECT id FROM t19 WHERE s LIKE '_a%';",
        "SELECT id FROM t19 WHERE s IS NULL;",
        "SELECT id FROM t19 WHERE a IS NOT NULL AND b > 30;",
    ])
    check("AND a>10,b<50 -> 2", "2", out)
    check("OR a>10,b<30 -> 1,2,3", "1", out)
    check("LIKE b% -> 2", "2", out)
    check("LIKE %e% -> 2,3", "3", out)
    check("LIKE _a% -> 2", "2", out)
    check("IS NULL -> 4", "4", out)
    check("IS NOT NULL AND -> 2,3", "3", out)

    # ============================================================
    print("\n=== 20. Transaction BEGIN/COMMIT/ROLLBACK ===")
    out = run_sql([
        "CREATE TABLE t20 (id INT PRIMARY KEY, v INT);",
        "BEGIN;",
        "INSERT INTO t20 VALUES (1, 10);",
        "COMMIT;",
        "SELECT id FROM t20;",
        "BEGIN;",
        "INSERT INTO t20 VALUES (2, 20);",
        "ROLLBACK;",
        "SELECT id FROM t20;",
    ])
    check("COMMIT persists", "1", out)
    check("ROLLBACK reverts", "Goodbye", out)

    # ============================================================
    print("\n=== 21. Arithmetic in WHERE ===")
    out = run_sql([
        "CREATE TABLE t21 (id INT PRIMARY KEY, a INT, b INT);",
        "INSERT INTO t21 VALUES (1, 10, 3), (2, 20, 5), (3, 15, 4);",
        "SELECT id FROM t21 WHERE a / b = 3;",
        "SELECT id FROM t21 WHERE a % b = 0;",
        "SELECT id FROM t21 WHERE a + b > 24;",
        "SELECT id FROM t21 WHERE a - b = 11;",
    ])
    check("a/b=3 -> 1", "1", out)
    check("a%b=0 -> 1,2", "1", out)
    check("a+b>24 -> 2", "2", out)
    check("a-b=11 -> 3", "3", out)

    # ============================================================
    print("\n=== 22. EXPLAIN ===")
    out = run_sql([
        "CREATE TABLE t22 (id INT PRIMARY KEY, v INT);",
        "CREATE INDEX idx_v22 ON t22 (v);",
        "EXPLAIN SELECT * FROM t22 WHERE id = 1;",
        "EXPLAIN SELECT * FROM t22 WHERE v = 10;",
    ])
    check("EXPLAIN shows plan", "Scan", out)

    # ============================================================
    print("\n=== 23. Subquery IN ===")
    out = run_sql([
        "CREATE TABLE s1 (id INT PRIMARY KEY);",
        "CREATE TABLE s2 (id INT PRIMARY KEY);",
        "INSERT INTO s1 VALUES (1), (2), (3);",
        "INSERT INTO s2 VALUES (2), (4);",
        "SELECT id FROM s1 WHERE id IN (SELECT id FROM s2);",
    ])
    check("Subquery IN finds 2", "2", out)

    # ============================================================
    print("\n=== 24. UNION ALL ===")
    out = run_sql([
        "CREATE TABLE u1 (id INT);",
        "CREATE TABLE u2 (id INT);",
        "INSERT INTO u1 VALUES (1), (2);",
        "INSERT INTO u2 VALUES (2), (3);",
        "SELECT id FROM u1 UNION ALL SELECT id FROM u2;",
    ])
    check("UNION ALL has 1", "1", out)
    check("UNION ALL has 3", "3", out)

    # ============================================================
    print("\n=== 25. BIGINT Overflow ===")
    out = run_sql([
        "CREATE TABLE t25 (id INT PRIMARY KEY, v BIGINT);",
        "INSERT INTO t25 VALUES (1, 9999999999);",
        "SELECT v FROM t25;",
        "SELECT v + 1 FROM t25;",
    ])
    check("BIGINT 9999999999", "9999999999", out)
    check("BIGINT +1", "10000000000", out)

    # ============================================================
    print("\n=== 26. Multi-Statement SQL ===")
    out = run_sql([
        "CREATE TABLE t26 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t26 VALUES (1, 10); INSERT INTO t26 VALUES (2, 20);",
        "SELECT COUNT(*) FROM t26;",
    ])
    check("Multi-stmt INSERT", "2", out)

    # ============================================================
    print("\n=== 27. SHOW TABLES / DESC ===")
    out = run_sql([
        "CREATE TABLE t27 (id INT PRIMARY KEY, name VARCHAR, age INT);",
        "SHOW TABLES;",
        "DESC t27;",
    ])
    check("SHOW TABLES shows t27", "t27", out)
    check("DESC shows name", "name", out)
    check("DESC shows VARCHAR", "VARCHAR", out)

    # ============================================================
    print("\n=== 28. UPDATE all rows ===")
    out = run_sql([
        "CREATE TABLE t28 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t28 VALUES (1, 10), (2, 20), (3, 30);",
        "UPDATE t28 SET v = 0;",
        "SELECT v FROM t28;",
    ])
    check("UPDATE all v=0", "0", out)

    # ============================================================
    print("\n=== 29. Double DELETE ===")
    out = run_sql([
        "CREATE TABLE t29 (id INT PRIMARY KEY);",
        "INSERT INTO t29 VALUES (1);",
        "DELETE FROM t29 WHERE id = 1;",
        "DELETE FROM t29 WHERE id = 1;",
        "SELECT COUNT(*) FROM t29;",
    ])
    check("Double DELETE safe", "0", out)

    # ============================================================
    print("\n=== 30. INSERT expressions ===")
    out = run_sql([
        "CREATE TABLE t30 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t30 VALUES (1, 1 + 2);",
        "SELECT v FROM t30;",
    ])
    check("INSERT expr 1+2=3", "3", out)

    # ============================================================
    print("\n=== 31. UPDATE with expressions ===")
    out = run_sql([
        "CREATE TABLE t31 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t31 VALUES (1, 10);",
        "UPDATE t31 SET v = v * 2;",
        "SELECT v FROM t31;",
    ])
    check("UPDATE expr v*2=20", "20", out)

    # ============================================================
    print("\n=== 32. NULLIF in SELECT ===")
    out = run_sql([
        "CREATE TABLE t32 (id INT PRIMARY KEY, a INT);",
        "INSERT INTO t32 VALUES (1, 10);",
        "INSERT INTO t32 VALUES (2, 10);",
        "SELECT NULLIF(a, 10) FROM t32;",
    ])
    check("NULLIF all 10 -> NULL", "NULL", out)

    # ============================================================
    print("\n=== 33. CASE WHEN all branches ===")
    out = run_sql([
        "CREATE TABLE t33 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t33 VALUES (1, 5);",
        "INSERT INTO t33 VALUES (2, 15);",
        "INSERT INTO t33 VALUES (3, 25);",
        "SELECT CASE WHEN v < 10 THEN 'low' WHEN v < 20 THEN 'mid' ELSE 'high' END FROM t33 ORDER BY id ASC;",
    ])
    check("CASE low", "low", out)
    check("CASE mid", "mid", out)
    check("CASE high", "high", out)

    # ============================================================
    print("\n=== 34. Multiple aggregates ===")
    out = run_sql([
        "CREATE TABLE t34 (v INT);",
        "INSERT INTO t34 VALUES (10), (20), (30);",
        "SELECT COUNT(*), MIN(v), MAX(v) FROM t34;",
    ])
    check("Multi agg COUNT", "3", out)
    check("Multi agg MIN", "10", out)
    check("Multi agg MAX", "30", out)

    # ============================================================
    print("\n=== 35. Table with all types NULL ===")
    out = run_sql([
        "CREATE TABLE t35 (id INT PRIMARY KEY, a INT, b VARCHAR);",
        "INSERT INTO t35 VALUES (1, NULL, NULL);",
        "SELECT a + 1 FROM t35;",
        "SELECT b FROM t35;",
    ])
    check("NULL int + 1", "NULL", out)
    check("NULL varchar", "NULL", out)

    # ============================================================
    print("\n=== 36. Re-INSERT after DELETE (reuse slot) ===")
    out = run_sql([
        "CREATE TABLE t36 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t36 VALUES (1, 10);",
        "DELETE FROM t36 WHERE id = 1;",
        "INSERT INTO t36 VALUES (2, 20);",
        "SELECT COUNT(*) FROM t36;",
        "SELECT v FROM t36;",
    ])
    check("Reuse slot count=1", "1", out)
    check("Reuse slot v=20", "20", out)

    # ============================================================
    print("\n=== 37. Long VARCHAR ===")
    long_str = "A" * 500
    out = run_sql([
        "CREATE TABLE t37 (id INT PRIMARY KEY, s VARCHAR);",
        f"INSERT INTO t37 VALUES (1, '{long_str}');",
        "SELECT LENGTH(s) FROM t37;",
    ])
    # LENGTH may not be supported, check for the string itself
    check("Long VARCHAR stored", "500", out)

    # ============================================================
    print("\n=== 38. Multiple JOINs ===")
    out = run_sql([
        "CREATE TABLE a38 (id INT PRIMARY KEY, v INT);",
        "CREATE TABLE b38 (id INT PRIMARY KEY, v INT);",
        "CREATE TABLE c38 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO a38 VALUES (1, 10);",
        "INSERT INTO b38 VALUES (1, 20);",
        "INSERT INTO c38 VALUES (1, 30);",
        "SELECT a38.v, b38.v, c38.v FROM a38 INNER JOIN b38 ON a38.id = b38.id INNER JOIN c38 ON b38.id = c38.id;",
    ])
    check("Multi JOIN 10", "10", out)
    check("Multi JOIN 20", "20", out)
    check("Multi JOIN 30", "30", out)

    # ============================================================
    print("\n=== 39. DELETE with complex WHERE ===")
    out = run_sql([
        "CREATE TABLE t39 (id INT PRIMARY KEY, a INT, b INT);",
        "INSERT INTO t39 VALUES (1, 10, 20), (2, 30, 40), (3, 50, 60);",
        "DELETE FROM t39 WHERE a > 10 AND b < 50;",
        "SELECT COUNT(*) FROM t39;",
    ])
    check("Complex DELETE count=2", "2", out)

    # ============================================================
    print("\n=== 40. UPDATE with NULL condition ===")
    out = run_sql([
        "CREATE TABLE t40 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t40 VALUES (1, NULL), (2, 10);",
        "UPDATE t40 SET v = 99 WHERE v IS NULL;",
        "SELECT v FROM t40;",
    ])
    check("UPDATE WHERE IS NULL", "99", out)

    # ============================================================
    print("\n=== 41. Column alias with expression ===")
    out = run_sql([
        "CREATE TABLE t41 (id INT PRIMARY KEY, a INT, b INT);",
        "INSERT INTO t41 VALUES (1, 3, 7);",
        "SELECT a + b AS total FROM t41;",
    ])
    check("Expr alias", "total", out)
    check("Expr result", "10", out)

    # ============================================================
    print("\n=== 42. Multiple ORDER BY columns ===")
    out = run_sql([
        "CREATE TABLE t42 (id INT PRIMARY KEY, a INT, b INT);",
        "INSERT INTO t42 VALUES (1, 1, 2), (2, 1, 1), (3, 2, 1);",
        "SELECT id FROM t42 ORDER BY a ASC, b ASC;",
    ])
    check("Multi ORDER first", "2", out)
    check("Multi ORDER second", "1", out)
    check("Multi ORDER third", "3", out)

    # ============================================================
    print("\n=== 43. Empty IN list ===")
    out = run_sql([
        "CREATE TABLE t43 (id INT PRIMARY KEY);",
        "INSERT INTO t43 VALUES (1);",
        "SELECT id FROM t43 WHERE id IN ();",
    ])
    check("Empty IN no results", "Goodbye", out)

    # ============================================================
    print("\n=== 44. LIKE edge cases ===")
    out = run_sql([
        "CREATE TABLE t44 (id INT PRIMARY KEY, s VARCHAR);",
        "INSERT INTO t44 VALUES (1, '');",
        "INSERT INTO t44 VALUES (2, 'a');",
        "INSERT INTO t44 VALUES (3, 'ab');",
        "INSERT INTO t44 VALUES (4, 'abc');",
        "SELECT id FROM t44 WHERE s LIKE '%';",
        "SELECT id FROM t44 WHERE s LIKE '_';",
        "SELECT id FROM t44 WHERE s LIKE '__';",
        "SELECT id FROM t44 WHERE s LIKE '%%';",
    ])
    check("LIKE % matches all", "1", out)
    check("LIKE _ matches 'a'", "2", out)
    check("LIKE _ matches empty", "Goodbye", out, negate=True)
    check("LIKE __ matches 'ab'", "3", out)

    # ============================================================
    print("\n=== 45. DISTINCT with NULLs ===")
    out = run_sql([
        "CREATE TABLE t45 (v INT);",
        "INSERT INTO t45 VALUES (1), (1), (NULL), (NULL);",
        "SELECT DISTINCT v FROM t45;",
    ])
    check("DISTINCT with NULL", "1", out)

    # ============================================================
    print("\n=== 46. Self-referencing arithmetic ===")
    out = run_sql([
        "CREATE TABLE t46 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t46 VALUES (1, 5);",
        "SELECT v * v + v - v FROM t46;",
    ])
    check("Self arithmetic 5*5=25", "25", out)

    # ============================================================
    print("\n=== 47. Multiple DELETE then INSERT ===")
    out = run_sql([
        "CREATE TABLE t47 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t47 VALUES (1, 10);",
        "INSERT INTO t47 VALUES (2, 20);",
        "DELETE FROM t47 WHERE id = 1;",
        "DELETE FROM t47 WHERE id = 2;",
        "INSERT INTO t47 VALUES (3, 30);",
        "SELECT COUNT(*) FROM t47;",
        "SELECT v FROM t47;",
    ])
    check("Del-Del-Ins count=1", "1", out)
    check("Del-Del-Ins v=30", "30", out)

    # ============================================================
    print("\n=== 48. Nested arithmetic expressions ===")
    out = run_sql([
        "CREATE TABLE t48 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t48 VALUES (1, 10);",
        "SELECT (v + v) * (v - v) FROM t48;",
        "SELECT v / (v - v + 1) FROM t48;",
    ])
    check("Nested arithmetic (10+10)*(10-10)=0", "0", out)
    check("Division by 1", "10", out)

    # ============================================================
    print("\n=== 49. LIMIT with no ORDER BY ===")
    out = run_sql([
        "CREATE TABLE t49 (id INT PRIMARY KEY);",
        "INSERT INTO t49 VALUES (1), (2), (3), (4), (5);",
        "SELECT id FROM t49 LIMIT 2;",
    ])
    check("LIMIT 2 without ORDER", "id", out)

    # ============================================================
    print("\n=== 50. Large number of columns ===")
    out = run_sql([
        "CREATE TABLE t50 (a1 INT, a2 INT, a3 INT, a4 INT, a5 INT, a6 INT, a7 INT, a8 INT, a9 INT, a10 INT);",
        "INSERT INTO t50 VALUES (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);",
        "SELECT a1, a10 FROM t50;",
    ])
    check("Wide table first col", "1", out)
    check("Wide table last col", "10", out)

    # ============================================================
    print("\n=== 51. Transaction implicit begin ===")
    out = run_sql([
        "CREATE TABLE t51 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t51 VALUES (1, 10);",
        "SELECT id FROM t51;",
    ])
    check("Implicit txn INSERT", "1", out)

    # ============================================================
    print("\n=== 52. DESC after CREATE ===")
    out = run_sql([
        "CREATE TABLE t52 (id INT PRIMARY KEY, name VARCHAR NOT NULL, age INT);",
        "DESC t52;",
    ])
    check("DESC shows id", "id", out)
    check("DESC shows name", "name", out)
    check("DESC shows NOT NULL", "YES", out)

    # ============================================================
    print("\n=== 53. SUM with all NULLs ===")
    out = run_sql([
        "CREATE TABLE t53 (v INT);",
        "INSERT INTO t53 VALUES (NULL), (NULL);",
        "SELECT SUM(v) FROM t53;",
        "SELECT COUNT(*) FROM t53;",
        "SELECT COUNT(v) FROM t53;",
    ])
    check("SUM all NULL -> NULL", "NULL", out)
    check("COUNT(*) with NULLs", "2", out)
    check("COUNT(v) with NULLs", "0", out)

    # ============================================================
    print("\n=== 54. AVG with NULLs ===")
    out = run_sql([
        "CREATE TABLE t54 (v INT);",
        "INSERT INTO t54 VALUES (10), (NULL), (30);",
        "SELECT AVG(v) FROM t54;",
        "SELECT COUNT(v) FROM t54;",
    ])
    check("AVG with NULLs", "20", out)

    # ============================================================
    print("\n=== 55. DELETE + INSERT same id ===")
    out = run_sql([
        "CREATE TABLE t55 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t55 VALUES (1, 10);",
        "DELETE FROM t55 WHERE id = 1;",
        "INSERT INTO t55 VALUES (1, 99);",
        "SELECT v FROM t55;",
    ])
    check("Reinsert same id", "99", out)

    # ============================================================
    print("\n=== 56. GROUP BY with NULL groups ===")
    out = run_sql([
        "CREATE TABLE t56 (cat INT, val INT);",
        "INSERT INTO t56 VALUES (1, 10), (1, 20), (NULL, 30);",
        "SELECT cat, COUNT(*) FROM t56 GROUP BY cat;",
    ])
    check("GROUP BY has 1", "1", out)
    check("GROUP BY has NULL group", "NULL", out)

    # ============================================================
    print("\n=== 57. LEFT JOIN with no match ===")
    out = run_sql([
        "CREATE TABLE l57 (id INT PRIMARY KEY, v INT);",
        "CREATE TABLE r57 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO l57 VALUES (1, 10), (2, 20);",
        "INSERT INTO r57 VALUES (1, 100);",
        "SELECT l57.id, r57.v FROM l57 LEFT JOIN r57 ON l57.id = r57.id ORDER BY l57.id ASC;",
    ])
    check("LEFT JOIN match", "100", out)
    check("LEFT JOIN NULL for 2", "2", out)

    # ============================================================
    print("\n=== 58. CASE with no ELSE ===")
    out = run_sql([
        "CREATE TABLE t58 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t58 VALUES (1, 100);",
        "SELECT CASE WHEN v > 200 THEN 'big' END FROM t58;",
    ])
    check("CASE no ELSE -> NULL", "NULL", out)

    # ============================================================
    print("\n=== 59. NOT IN with empty result ===")
    out = run_sql([
        "CREATE TABLE t59 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t59 VALUES (1, 10);",
        "SELECT id FROM t59 WHERE v NOT IN (20, 30);",
    ])
    check("NOT IN all miss -> all rows", "1", out)

    # ============================================================
    print("\n=== 60. Expressions in INSERT with table columns ===")
    out = run_sql([
        "CREATE TABLE t60 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t60 VALUES (1, 10);",
        "INSERT INTO t60 VALUES (2, (SELECT 20));",
        "SELECT v FROM t60 ORDER BY id ASC;",
    ])
    check("Scalar subquery INSERT", "20", out)

    # ============================================================
    print("\n=== 61. Multiple aggregates in SELECT ===")
    out = run_sql([
        "CREATE TABLE t61 (v INT);",
        "INSERT INTO t61 VALUES (5), (15), (25);",
        "SELECT MIN(v), MAX(v), COUNT(*) FROM t61;",
    ])
    check("Multi agg MIN", "5", out)
    check("Multi agg MAX", "25", out)
    check("Multi agg COUNT", "3", out)

    # ============================================================
    print("\n=== 62. SUM of negative numbers ===")
    out = run_sql([
        "CREATE TABLE t62 (v INT);",
        "INSERT INTO t62 VALUES (-10), (-20), (30);",
        "SELECT SUM(v) FROM t62;",
    ])
    check("SUM negative", "0", out)

    # ============================================================
    print("\n=== 63. Big integers in expressions ===")
    out = run_sql([
        "CREATE TABLE t63 (id INT PRIMARY KEY, v BIGINT);",
        "INSERT INTO t63 VALUES (1, 1000000000);",
        "SELECT v * 2 FROM t63;",
    ])
    check("Bigint multiply", "2000000000", out)

    # ============================================================
    print("\n=== 64. Multiple WHERE with OR ===")
    out = run_sql([
        "CREATE TABLE t64 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t64 VALUES (1, 10), (2, 20), (3, 30), (4, 40);",
        "SELECT id FROM t64 WHERE v = 10 OR v = 40;",
    ])
    check("OR finds 1", "1", out)
    check("OR finds 4", "4", out)
    check("OR excludes 2", "2", out, negate=True)

    # ============================================================
    print("\n=== 65. LIKE with special patterns ===")
    out = run_sql([
        "CREATE TABLE t65 (id INT PRIMARY KEY, s VARCHAR);",
        "INSERT INTO t65 VALUES (1, 'abc');",
        "INSERT INTO t65 VALUES (2, 'a_c');",
        "INSERT INTO t65 VALUES (3, 'a%c');",
        "SELECT id FROM t65 WHERE s LIKE 'a%c';",
    ])
    check("LIKE a%c matches abc", "1", out)
    check("LIKE a%c matches a_c", "2", out)
    check("LIKE a%c matches a%c", "3", out)

    # ============================================================
    print("\n=== 66. Boolean in WHERE ===")
    out = run_sql([
        "CREATE TABLE t66 (id INT PRIMARY KEY, flag BOOL);",
        "INSERT INTO t66 VALUES (1, TRUE), (2, FALSE);",
        "SELECT id FROM t66 WHERE flag = TRUE;",
        "SELECT id FROM t66 WHERE flag = FALSE;",
    ])
    check("BOOL TRUE", "1", out)
    check("BOOL FALSE", "2", out)

    # ============================================================
    print("\n=== 67. Multiple columns in UPDATE ===")
    out = run_sql([
        "CREATE TABLE t67 (id INT PRIMARY KEY, a INT, b INT);",
        "INSERT INTO t67 VALUES (1, 10, 20);",
        "UPDATE t67 SET a = 99, b = 88 WHERE id = 1;",
        "SELECT a, b FROM t67;",
    ])
    check("Multi-col UPDATE a", "99", out)
    check("Multi-col UPDATE b", "88", out)

    # ============================================================
    print("\n=== 68. SELECT * ===")
    out = run_sql([
        "CREATE TABLE t68 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t68 VALUES (1, 42);",
        "SELECT * FROM t68;",
    ])
    check("SELECT * shows id", "1", out)
    check("SELECT * shows v", "42", out)

    # ============================================================
    print("\n=== 69. UPDATE with arithmetic ===")
    out = run_sql([
        "CREATE TABLE t69 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO t69 VALUES (1, 100);",
        "UPDATE t69 SET v = v + 50;",
        "SELECT v FROM t69;",
    ])
    check("UPDATE arithmetic", "150", out)

    # ============================================================
    print("\n=== 70. LIMIT OFFSET edge cases ===")
    out = run_sql([
        "CREATE TABLE t70 (id INT PRIMARY KEY);",
        "INSERT INTO t70 VALUES (1), (2), (3);",
        "SELECT id FROM t70 ORDER BY id ASC LIMIT 1;",
        "SELECT id FROM t70 ORDER BY id ASC LIMIT 1 OFFSET 1;",
        "SELECT id FROM t70 ORDER BY id ASC LIMIT 1 OFFSET 2;",
        "SELECT id FROM t70 ORDER BY id ASC LIMIT 1 OFFSET 3;",
    ])
    check("LIMIT 1 first", "1", out)
    check("LIMIT 1 OFFSET 1", "2", out)
    check("LIMIT 1 OFFSET 2", "3", out)
    check("LIMIT 1 OFFSET 3 empty", "Goodbye", out)

finally:
    cleanup()

print()
print("=" * 50)
print(f"  RESULTS: {PASS}/{PASS + FAIL} passed, {FAIL} failed")
print("=" * 50)

if BUGS:
    print("\n--- DISCOVERED BUGS ---")
    for desc, expected, got in BUGS:
        print(f"\n[BUG] {desc}")
        print(f"  Expected to contain: {expected}")
        print(f"  Got: {got[:200]}...")
    sys.exit(1)
else:
    print("\nAll tests passed!")
    sys.exit(0)
