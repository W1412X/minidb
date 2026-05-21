#!/usr/bin/env python3
"""Ultimate SQL exercise — every syntax combination and boundary case."""
import subprocess, tempfile, shutil, sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
PASS = FAIL = 0
BUGS = []

def run(sqls, db=None):
    if db is None: db = tempfile.mkdtemp()
    if isinstance(sqls, str): sqls = [sqls]
    sqls.append("exit")
    r = subprocess.run([BIN, "--dir", db], input="\n".join(sqls),
                       capture_output=True, text=True, timeout=60)
    return r.stdout + r.stderr

def check(desc, needle, out, negate=False):
    global PASS, FAIL
    found = needle in out
    if negate: found = not found
    if found:
        PASS += 1
    else:
        FAIL += 1
        BUGS.append((desc, needle))
        print(f"  FAIL: {desc}")

# =================================================================
print("=== 1. DDL ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT NOT NULL, email VARCHAR UNIQUE);", db)
    check("CREATE TABLE", "t", run("SHOW TABLES;", db))
    check("DESC PK", "YES", run("DESC t;", db))
    check("DESC NOT NULL", "YES", run("DESC t;", db))
    check("DESC UNIQUE", "YES", run("DESC t;", db))
    run("CREATE TABLE t2 (id INT PRIMARY KEY);", db)
    run("DROP TABLE t2;", db)
    check("DROP TABLE gone", "t2", run("SHOW TABLES;", db), negate=True)
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 2. INSERT variants ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT, name VARCHAR);", db)
    run("INSERT INTO t VALUES (1, 10, 'a');", db)
    check("Single row", "a", run("SELECT name FROM t;", db))
    run("INSERT INTO t VALUES (2, 20, 'b'), (3, 30, 'c');", db)
    check("Multi row", "3", run("SELECT COUNT(*) FROM t;", db))
    run("INSERT INTO t (id, name) VALUES (4, 'd');", db)
    check("Partial", "NULL", run("SELECT v FROM t WHERE id = 4;", db))
    run("INSERT INTO t VALUES (5, 10+20, 'e');", db)
    check("Expr", "30", run("SELECT v FROM t WHERE id = 5;", db))
    run("INSERT INTO t VALUES (6, NULL, NULL);", db)
    check("NULL", "NULL", run("SELECT v FROM t WHERE id = 6;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 3. UPDATE variants ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);", db)
    run("UPDATE t SET v = 99 WHERE id = 1;", db)
    check("SET literal", "99", run("SELECT v FROM t WHERE id = 1;", db))
    run("UPDATE t SET v = v + 1 WHERE id = 2;", db)
    check("SET expr", "21", run("SELECT v FROM t WHERE id = 2;", db))
    run("UPDATE t SET v = NULL WHERE id = 3;", db)
    check("SET NULL", "NULL", run("SELECT v FROM t WHERE id = 3;", db))
    run("UPDATE t SET v = 88 WHERE v IS NULL;", db)
    check("WHERE IS NULL", "88", run("SELECT v FROM t WHERE id = 3;", db))
    run("UPDATE t SET v = v + 10;", db)
    check("UPDATE all", "10", run("SELECT v FROM t WHERE id = 1;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 4. DELETE variants ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);", db)
    run("DELETE FROM t WHERE id = 2;", db)
    check("DELETE WHERE", "2", run("SELECT COUNT(*) FROM t;", db))
    run("DELETE FROM t;", db)
    check("DELETE all", "0", run("SELECT COUNT(*) FROM t;", db))
    run("INSERT INTO t VALUES (1, 10);", db)
    check("INSERT after DELETE", "10", run("SELECT v FROM t;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 5. SELECT WHERE ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT, s VARCHAR);", db)
    run("INSERT INTO t VALUES (1, 10, 20, 'hello'), (2, 30, 40, 'world'), (3, 50, 60, 'foo');", db)
    check("WHERE =", "world", run("SELECT s FROM t WHERE id = 2;", db))
    check("WHERE !=", "hello", run("SELECT s FROM t WHERE id != 2 LIMIT 1;", db))
    check("WHERE <", "hello", run("SELECT s FROM t WHERE a < 30;", db))
    check("WHERE >", "foo", run("SELECT s FROM t WHERE a > 30;", db))
    check("WHERE >=", "world", run("SELECT s FROM t WHERE a >= 30;", db))
    check("WHERE <=", "hello", run("SELECT s FROM t WHERE a <= 10;", db))
    check("WHERE AND", "world", run("SELECT s FROM t WHERE a > 10 AND b < 50;", db))
    check("WHERE OR", "hello", run("SELECT s FROM t WHERE a = 10 OR a = 50;", db))
    check("WHERE NOT", "hello", run("SELECT s FROM t WHERE NOT a > 10;", db))
    check("WHERE IN", "world", run("SELECT s FROM t WHERE id IN (2);", db))
    check("WHERE NOT IN", "hello", run("SELECT s FROM t WHERE id NOT IN (2, 3);", db))
    check("WHERE BETWEEN", "world", run("SELECT s FROM t WHERE a BETWEEN 20 AND 40;", db))
    check("WHERE LIKE", "hello", run("SELECT s FROM t WHERE s LIKE 'h%';", db))
    check("WHERE IS NULL", "foo", run("SELECT s FROM t WHERE b IS NOT NULL AND id = 3;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 6. ORDER BY / LIMIT / OFFSET ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 30), (2, 10), (3, 20), (4, 5), (5, 40);", db)
    check("ORDER ASC first", "4", run("SELECT id FROM t ORDER BY v ASC LIMIT 1;", db))
    check("ORDER DESC first", "5", run("SELECT id FROM t ORDER BY v DESC LIMIT 1;", db))
    check("LIMIT", "3", run("SELECT COUNT(*) FROM (SELECT id FROM t ORDER BY v LIMIT 3);", db))
    check("OFFSET", "2", run("SELECT id FROM t ORDER BY v ASC LIMIT 1 OFFSET 1;", db))
    check("DISTINCT", "5", run("SELECT COUNT(DISTINCT v) FROM t;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 7. Aggregates + GROUP BY ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (cat VARCHAR, v INT);", db)
    run("INSERT INTO t VALUES ('A', 10), ('A', 20), ('B', 30), ('B', 40);", db)
    check("COUNT(*)", "4", run("SELECT COUNT(*) FROM t;", db))
    check("SUM", "100", run("SELECT SUM(v) FROM t;", db))
    check("MIN", "10", run("SELECT MIN(v) FROM t;", db))
    check("MAX", "40", run("SELECT MAX(v) FROM t;", db))
    check("AVG", "25", run("SELECT AVG(v) FROM t;", db))
    check("COUNT WHERE", "2", run("SELECT COUNT(*) FROM t WHERE cat = 'A';", db))
    check("SUM WHERE", "30", run("SELECT SUM(v) FROM t WHERE cat = 'A';", db))
    check("GROUP BY + HAVING", "B", run("SELECT cat FROM t GROUP BY cat HAVING MAX(v) > 25;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 8. JOIN ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE j1 (id INT PRIMARY KEY, v INT);", db)
    run("CREATE TABLE j2 (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO j1 VALUES (1, 10), (2, 20), (3, 30);", db)
    run("INSERT INTO j2 VALUES (1, 100), (3, 300);", db)
    check("INNER JOIN", "2", run("SELECT COUNT(*) FROM j1 INNER JOIN j2 ON j1.id = j2.id;", db))
    check("LEFT JOIN", "3", run("SELECT COUNT(*) FROM j1 LEFT JOIN j2 ON j1.id = j2.id;", db))
    check("CROSS JOIN", "6", run("SELECT COUNT(*) FROM j1 CROSS JOIN j2;", db))
    check("JOIN WHERE", "10", run("SELECT j1.v FROM j1 INNER JOIN j2 ON j1.id = j2.id WHERE j1.id = 1;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 9. NULL handling ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT, s VARCHAR);", db)
    run("INSERT INTO t VALUES (1, NULL, NULL), (2, 10, 'hello'), (3, NULL, 'world');", db)
    check("IS NULL", "2", run("SELECT COUNT(*) FROM t WHERE v IS NULL;", db))
    check("IS NOT NULL", "1", run("SELECT COUNT(*) FROM t WHERE v IS NOT NULL;", db))
    check("v=NULL", "Goodbye", run("SELECT id FROM t WHERE v = NULL;", db))
    check("v!=NULL", "Goodbye", run("SELECT id FROM t WHERE v != NULL;", db))
    check("COUNT(*)", "3", run("SELECT COUNT(*) FROM t;", db))
    check("COUNT(v)", "1", run("SELECT COUNT(v) FROM t;", db))
    check("SUM ignores NULL", "10", run("SELECT SUM(v) FROM t;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 10. Transactions ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10);", db)
    run("BEGIN; INSERT INTO t VALUES (2, 20); COMMIT;", db)
    check("COMMIT", "2", run("SELECT COUNT(*) FROM t;", db))
    run("BEGIN; INSERT INTO t VALUES (3, 30); ROLLBACK;", db)
    check("ROLLBACK", "2", run("SELECT COUNT(*) FROM t;", db))
    run("BEGIN; UPDATE t SET v = 99 WHERE id = 1; ROLLBACK;", db)
    check("UPDATE ROLLBACK", "10", run("SELECT v FROM t WHERE id = 1;", db))
    run("BEGIN; DELETE FROM t WHERE id = 2; ROLLBACK;", db)
    check("DELETE ROLLBACK", "2", run("SELECT COUNT(*) FROM t;", db))
    run("BEGIN; INSERT INTO t VALUES (4, 40); COMMIT;", db)
    check("COMMIT after ROLLBACK", "3", run("SELECT COUNT(*) FROM t;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 11. Expressions ===")
db = tempfile.mkdtemp()
try:
    check("1+2*3=7", "7", run("SELECT 1 + 2 * 3;", db))
    check("(1+2)*3=9", "9", run("SELECT (1 + 2) * 3;", db))
    check("-1", "-1", run("SELECT -1;", db))
    check("10/3=3", "3", run("SELECT 10 / 3;", db))
    check("10%3=1", "1", run("SELECT 10 % 3;", db))
    check("1+NULL=NULL", "NULL", run("SELECT 1 + NULL;", db))
    check("NULL AND TRUE=NULL", "NULL", run("SELECT NULL AND TRUE;", db))
    check("NULL OR TRUE=1", "1", run("SELECT NULL OR TRUE;", db))
    check("NOT NULL=NULL", "NULL", run("SELECT NOT NULL;", db))
    check("10/0=NULL", "NULL", run("SELECT 10 / 0;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 12. CASE / COALESCE / NULLIF ===")
db = tempfile.mkdtemp()
try:
    check("CASE low", "low", run("SELECT CASE WHEN 5 < 10 THEN 'low' ELSE 'high' END;", db))
    check("CASE mid", "mid", run("SELECT CASE WHEN 15 < 10 THEN 'low' WHEN 15 < 20 THEN 'mid' ELSE 'high' END;", db))
    check("COALESCE", "20", run("SELECT COALESCE(NULL, 20);", db))
    check("COALESCE 3", "30", run("SELECT COALESCE(NULL, NULL, 30);", db))
    check("NULLIF equal", "NULL", run("SELECT NULLIF(10, 10);", db))
    check("NULLIF diff", "10", run("SELECT NULLIF(10, 20);", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 13. Indexes ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("CREATE INDEX idx_v ON t (v);", db)
    run("INSERT INTO t VALUES (1, 100), (2, 200), (3, 300);", db)
    check("Index eq", "2", run("SELECT id FROM t WHERE v = 200;", db))
    check("Index range", "1", run("SELECT COUNT(*) FROM t WHERE v > 150 AND v < 250;", db))
    run("UPDATE t SET v = 999 WHERE id = 1;", db)
    check("Index after UPDATE", "1", run("SELECT id FROM t WHERE v = 999;", db))
    run("DELETE FROM t WHERE v = 999;", db)
    check("Index after DELETE", "Goodbye", run("SELECT id FROM t WHERE v = 999;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 14. DDL: ALTER TABLE ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR);", db)
    run("INSERT INTO t VALUES (1, 'alice');", db)
    run("ALTER TABLE t ADD COLUMN age INT DEFAULT 25;", db)
    check("ADD COLUMN", "25", run("SELECT age FROM t;", db))
    run("ALTER TABLE t RENAME COLUMN name TO full_name;", db)
    check("RENAME", "full_name", run("DESC t;", db))
    run("ALTER TABLE t DROP COLUMN age;", db)
    check("DROP COLUMN", "age", run("DESC t;", db), negate=True)
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 15. Multi-statement ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10); INSERT INTO t VALUES (2, 20);", db)
    check("Multi INSERT", "2", run("SELECT COUNT(*) FROM t;", db))
    run("UPDATE t SET v = v + 1 WHERE id = 1; UPDATE t SET v = v + 1 WHERE id = 2;", db)
    check("Multi UPDATE", "21", run("SELECT v FROM t WHERE id = 2;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 16. Subqueries ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE a (id INT PRIMARY KEY);", db)
    run("CREATE TABLE b (id INT PRIMARY KEY);", db)
    run("INSERT INTO a VALUES (1), (2), (3);", db)
    run("INSERT INTO b VALUES (2), (4);", db)
    check("IN subquery", "2", run("SELECT id FROM a WHERE id IN (SELECT id FROM b);", db))
    check("NOT IN subquery", "1", run("SELECT id FROM a WHERE id NOT IN (SELECT id FROM b) LIMIT 1;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 17. Persistence ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10);", db)
    check("INSERT persist", "10", run("SELECT v FROM t;", db))
    run("UPDATE t SET v = 99 WHERE id = 1;", db)
    check("UPDATE persist", "99", run("SELECT v FROM t;", db))
    run("DELETE FROM t WHERE id = 1;", db)
    check("DELETE persist", "0", run("SELECT COUNT(*) FROM t;", db))
    run("ALTER TABLE t ADD COLUMN name VARCHAR DEFAULT 'x';", db)
    run("INSERT INTO t VALUES (2, 20, 'y');", db)
    check("ALTER persist", "y", run("SELECT name FROM t;", db))
    run("CREATE TABLE t2 (id INT PRIMARY KEY, v INT);", db)
    run("CREATE INDEX t2v ON t2 (v);", db)
    run("INSERT INTO t2 VALUES (1, 100);", db)
    check("INDEX persist", "1", run("SELECT id FROM t2 WHERE v = 100;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 18. UPDATE persistence (multiple) ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10);", db)
    run("UPDATE t SET v = v + 10;", db)
    check("1st UPDATE", "20", run("SELECT v FROM t;", db))
    run("UPDATE t SET v = v + 10;", db)
    check("2nd UPDATE", "30", run("SELECT v FROM t;", db))
    run("UPDATE t SET v = v + 10;", db)
    check("3rd UPDATE", "40", run("SELECT v FROM t;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 19. Edge cases ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    check("Empty COUNT", "0", run("SELECT COUNT(*) FROM t;", db))
    check("Empty SUM NULL", "NULL", run("SELECT SUM(v) FROM t;", db))
    run("INSERT INTO t VALUES (1, 0);", db)
    check("Zero value", "0", run("SELECT v FROM t;", db))
    run("CREATE TABLE neg (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO neg VALUES (1, -999);", db)
    check("Negative", "-999", run("SELECT v FROM neg;", db))
    run("CREATE TABLE big (id INT PRIMARY KEY, v BIGINT);", db)
    run("INSERT INTO big VALUES (1, 9999999999);", db)
    check("Big number", "9999999999", run("SELECT v FROM big;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n=== 20. Stress: bulk operations ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    for i in range(1, 101):
        run(f"INSERT INTO t VALUES ({i}, {i});", db)
    check("100 INSERTs count", "100", run("SELECT COUNT(*) FROM t;", db))
    check("100 INSERTs sum", "5050", run("SELECT SUM(v) FROM t;", db))
    run("DELETE FROM t WHERE v > 50;", db)
    check("DELETE half", "50", run("SELECT COUNT(*) FROM t;", db))
    run("UPDATE t SET v = v * 10;", db)
    check("UPDATE all", "10", run("SELECT v FROM t WHERE id = 1;", db))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n" + "=" * 50)
print(f"  RESULTS: {PASS}/{PASS+FAIL} passed, {FAIL} failed")
print("=" * 50)
if BUGS:
    print("\n--- BUGS ---")
    for desc, needle in BUGS:
        print(f"  {desc} (expected '{needle}')")
    sys.exit(1)
else:
    print("\nAll tests passed!")
    sys.exit(0)
