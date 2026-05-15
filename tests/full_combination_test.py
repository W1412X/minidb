#!/usr/bin/env python3
"""
全面 SQL 组合测试 — 所有支持的关键词的所有组合，边界条件
"""
import subprocess, tempfile, shutil, sys, os

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
PASS = FAIL = 0
BUGS = []

def run(sqls, db=None):
    """Run SQL in minidb and return (stdout, stderr)"""
    if db is None:
        db = tempfile.mkdtemp()
        cleanup = True
    else:
        cleanup = False
    if isinstance(sqls, str): sqls = [sqls]
    sqls.append("exit")
    r = subprocess.run([BIN, "--dir", db], input="\n".join(sqls),
                       capture_output=True, text=True, timeout=60)
    if cleanup:
        shutil.rmtree(db, ignore_errors=True)
    return r.stdout + r.stderr

def runf(db, sql):
    """Run single SQL, return clean output (no prompts/noise)"""
    out = run(sql, db)
    lines = []
    for l in out.split('\n'):
        s = l.strip()
        if s.startswith('minidb> '): s = s[8:]
        if s and not any(s.startswith(p) for p in
                         ['Data dir', 'MiniADB', 'Type ', '[DB]']):
            if s not in ('Goodbye.', 'Goodbye', 'Tables:'):
                lines.append(s)
    return '\n'.join(lines)

def chk(desc, needle, out, negate=False):
    global PASS, FAIL
    found = needle in out
    if negate: found = not found
    if found:
        PASS += 1
    else:
        FAIL += 1
        BUGS.append((desc, needle, out[:200]))
        print(f"  FAIL: {desc}")

def setup(db, table_defs, inserts):
    """Create tables and insert data"""
    run(table_defs, db)
    for ins in inserts:
        run(ins, db)

# =================================================================
# 1. CREATE TABLE — 所有类型 + 约束组合
# =================================================================
print("=== 1. CREATE TABLE 类型 + 约束组合 ===")
db = tempfile.mkdtemp()
try:
    # 单类型
    run("CREATE TABLE t1 (id INT PRIMARY KEY);", db)
    chk("INT PRIMARY KEY", "INT", runf(db, "DESC t1;"))
    
    run("CREATE TABLE t2 (id INT PRIMARY KEY, v BIGINT);", db)
    chk("BIGINT", "BIGINT", runf(db, "DESC t2;"))
    
    run("CREATE TABLE t3 (id INT PRIMARY KEY, v DOUBLE);", db)
    chk("DOUBLE", "DOUBLE", runf(db, "DESC t3;"))
    
    run("CREATE TABLE t4 (id INT PRIMARY KEY, v VARCHAR);", db)
    chk("VARCHAR", "VARCHAR", runf(db, "DESC t4;"))
    
    # 多约束
    run("CREATE TABLE t5 (id INT PRIMARY KEY, a VARCHAR NOT NULL, b INT UNIQUE, c VARCHAR);", db)
    chk("NOT NULL", "YES", runf(db, "DESC t5;"))
    chk("UNIQUE", "YES", runf(db, "DESC t5;"))
    
    # 空表
    run("CREATE TABLE t6 (id INT PRIMARY KEY);", db)
    chk("Empty table: COUNT=0", "0", runf(db, "SELECT COUNT(*) FROM t6;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 2. INSERT — 所有变体
# =================================================================
print("\n=== 2. INSERT 变体 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT, name VARCHAR);", db)
    
    # 单行
    run("INSERT INTO t VALUES (1, 10, 'a');", db)
    chk("Single row", "a", runf(db, "SELECT name FROM t;"))
    
    # 多行
    run("INSERT INTO t VALUES (2, 20, 'b'), (3, 30, 'c');", db)
    chk("Multi row", "3", runf(db, "SELECT COUNT(*) FROM t;"))
    
    # 指定列
    run("INSERT INTO t (id, name) VALUES (4, 'd');", db)
    chk("Partial columns", "NULL", runf(db, "SELECT v FROM t WHERE id = 4;"))
    
    # 表达式
    run("INSERT INTO t VALUES (5, 10+20, 'e');", db)
    chk("INSERT expr", "30", runf(db, "SELECT v FROM t WHERE id = 5;"))
    
    # NULL
    run("INSERT INTO t VALUES (6, NULL, NULL);", db)
    chk("INSERT NULL", "NULL", runf(db, "SELECT v FROM t WHERE id = 6;"))
    
    # 多列指定
    run("INSERT INTO t (id, name) VALUES (7, 'g'), (8, 'h');", db)
    chk("Multi partial", "8", runf(db, "SELECT COUNT(*) FROM t WHERE name IS NOT NULL;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 3. UPDATE — 所有变体
# =================================================================
print("\n=== 3. UPDATE 变体 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT, name VARCHAR);", db)
    run("INSERT INTO t VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c');", db)
    
    # SET literal
    run("UPDATE t SET v = 99 WHERE id = 1;", db)
    chk("SET literal", "99", runf(db, "SELECT v FROM t WHERE id = 1;"))
    
    # SET expression
    run("UPDATE t SET v = v + 10 WHERE id = 2;", db)
    chk("SET expr", "30", runf(db, "SELECT v FROM t WHERE id = 2;"))
    
    # SET NULL
    run("UPDATE t SET v = NULL WHERE id = 3;", db)
    chk("SET NULL", "NULL", runf(db, "SELECT v FROM t WHERE id = 3;"))
    
    # SET WHERE IS NULL
    run("UPDATE t SET v = 99 WHERE v IS NULL;", db)
    chk("UPDATE WHERE NULL", "99", runf(db, "SELECT v FROM t WHERE id = 3;"))
    
    # SET multiple columns
    run("UPDATE t SET v = v + 1, name = 'updated' WHERE id = 1;", db)
    chk("SET multi", "updated", runf(db, "SELECT name FROM t WHERE id = 1;"))
    
    # SET all rows (no WHERE)
    run("UPDATE t SET v = v * 2;", db)
    chk("UPDATE all", "200", runf(db, "SELECT v FROM t WHERE id = 1;"))
    
    # SET with complex WHERE
    run("UPDATE t SET v = 100 WHERE id > 1 AND name = 'updated';", db)
    chk("UPDATE complex WHERE", "30", runf(db, "SELECT v FROM t WHERE id = 2;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 4. DELETE — 所有变体
# =================================================================
print("\n=== 4. DELETE 变体 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40);", db)
    
    # WHERE clause
    run("DELETE FROM t WHERE id = 2;", db)
    chk("DELETE WHERE", "3", runf(db, "SELECT COUNT(*) FROM t;"))
    
    # DELETE + re-INSERT
    run("INSERT INTO t VALUES (5, 50);", db)
    chk("DELETE + INSERT", "5", runf(db, "SELECT id FROM t WHERE v = 50;"))
    
    # DELETE all
    run("DELETE FROM t;", db)
    chk("DELETE all", "0", runf(db, "SELECT COUNT(*) FROM t;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 5. SELECT — 所有组合
# =================================================================
print("\n=== 5. SELECT 组合 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT, c VARCHAR);", db)
    run("INSERT INTO t VALUES (1, 10, 20, 'x'), (2, 30, 40, 'y'), (3, 50, 60, 'z');", db)
    
    # 列选择
    chk("SELECT *", "10", runf(db, "SELECT * FROM t;"))
    chk("SELECT col", "x", runf(db, "SELECT c FROM t WHERE id = 1;"))
    chk("SELECT multi", "10", runf(db, "SELECT a, b FROM t WHERE id = 1;"))
    
    # WHERE
    chk("WHERE =", "y", runf(db, "SELECT c FROM t WHERE id = 2;"))
    chk("WHERE !=", "x", runf(db, "SELECT c FROM t WHERE id != 2;"))
    chk("WHERE <", "x", runf(db, "SELECT c FROM t WHERE a < 30;"))
    chk("WHERE >", "z", runf(db, "SELECT c FROM t WHERE a > 30;"))
    chk("WHERE >=", "y", runf(db, "SELECT c FROM t WHERE a >= 30;"))
    chk("WHERE <=", "x", runf(db, "SELECT c FROM t WHERE a <= 10;"))
    
    # WHERE AND/OR
    chk("WHERE AND", "y", runf(db, "SELECT c FROM t WHERE a > 10 AND b < 50;"))
    chk("WHERE OR", "x", runf(db, "SELECT c FROM t WHERE a = 10 OR a = 50;"))
    chk("WHERE NOT", "x", runf(db, "SELECT c FROM t WHERE NOT a > 10;"))
    
    # WHERE IN
    chk("WHERE IN", "x", runf(db, "SELECT c FROM t WHERE id IN (1);"))
    chk("WHERE NOT IN", "z", runf(db, "SELECT c FROM t WHERE id NOT IN (1, 2);"))
    
    # WHERE BETWEEN
    chk("WHERE BETWEEN", "y", runf(db, "SELECT c FROM t WHERE a BETWEEN 20 AND 40;"))
    
    # WHERE LIKE
    chk("WHERE LIKE", "x", runf(db, "SELECT c FROM t WHERE c LIKE 'x%';"))
    chk("WHERE LIKE any", "x", runf(db, "SELECT c FROM t WHERE c LIKE '%';"))
    
    # ORDER BY
    chk("ORDER BY ASC", "4", runf(db, "SELECT id FROM t ORDER BY a ASC LIMIT 1;"))
    chk("ORDER BY DESC", "3", runf(db, "SELECT id FROM t ORDER BY a DESC LIMIT 1;"))
    
    # LIMIT + OFFSET
    chk("LIMIT", "1", runf(db, "SELECT id FROM t ORDER BY a LIMIT 1;"))
    chk("OFFSET", "2", runf(db, "SELECT id FROM t ORDER BY a LIMIT 1 OFFSET 1;"))
    
    # DISTINCT
    run("INSERT INTO t VALUES (4, 10, 20, 'x');", db)
    chk("DISTINCT", "3", runf(db, "SELECT COUNT(DISTINCT a) FROM t;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 6. 聚合函数
# =================================================================
print("\n=== 6. 聚合函数 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, NULL);", db)
    
    chk("COUNT(*)", "4", runf(db, "SELECT COUNT(*) FROM t;"))
    chk("COUNT(v)", "3", runf(db, "SELECT COUNT(v) FROM t;"))
    chk("SUM", "60", runf(db, "SELECT SUM(v) FROM t;"))
    chk("AVG", "20", runf(db, "SELECT AVG(v) FROM t;"))
    chk("MIN", "10", runf(db, "SELECT MIN(v) FROM t;"))
    chk("MAX", "30", runf(db, "SELECT MAX(v) FROM t;"))
    
    # 空表聚合
    run("CREATE TABLE empty (v INT);", db)
    chk("Empty COUNT", "0", runf(db, "SELECT COUNT(*) FROM empty;"))
    chk("Empty SUM NULL", "NULL", runf(db, "SELECT SUM(v) FROM empty;"))
    
    # 聚合 + WHERE
    chk("COUNT WHERE", "2", runf(db, "SELECT COUNT(*) FROM t WHERE v > 15;"))
    chk("SUM WHERE", "50", runf(db, "SELECT SUM(v) FROM t WHERE v > 15;"))
    
    # 多聚合
    chk("Multi agg", "4", runf(db, "SELECT COUNT(*) FROM t;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 7. GROUP BY + HAVING
# =================================================================
print("\n=== 7. GROUP BY ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (cat VARCHAR, v INT);", db)
    run("INSERT INTO t VALUES ('A', 10), ('A', 20), ('B', 30), ('B', 40), (NULL, 50);", db)
    
    chk("COUNT group", "2", runf(db, "SELECT COUNT(*) FROM t WHERE cat = 'A';"))
    chk("SUM group", "30", runf(db, "SELECT SUM(v) FROM t WHERE cat = 'A';"))
    chk("HAVING", "B", runf(db, "SELECT cat FROM t GROUP BY cat HAVING MAX(v) > 25;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 8. JOIN
# =================================================================
print("\n=== 8. JOIN ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE j1 (id INT PRIMARY KEY, v INT);", db)
    run("CREATE TABLE j2 (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO j1 VALUES (1, 10), (2, 20), (3, 30);", db)
    run("INSERT INTO j2 VALUES (1, 100), (3, 300);", db)
    
    # INNER JOIN
    chk("INNER JOIN count", "2", runf(db, "SELECT COUNT(*) FROM j1 INNER JOIN j2 ON j1.id = j2.id;"))
    
    # LEFT JOIN
    chk("LEFT JOIN count", "3", runf(db, "SELECT COUNT(*) FROM j1 LEFT JOIN j2 ON j1.id = j2.id;"))
    
    # CROSS JOIN
    chk("CROSS JOIN count", "6", runf(db, "SELECT COUNT(*) FROM j1 CROSS JOIN j2;"))
    
    # JOIN + WHERE
    chk("JOIN WHERE", "10", runf(db, "SELECT j1.v FROM j1 INNER JOIN j2 ON j1.id = j2.id WHERE j1.id = 1;"))
    
    # JOIN + ORDER BY
    chk("JOIN ORDER", "30", runf(db, "SELECT j1.v FROM j1 INNER JOIN j2 ON j1.id = j2.id ORDER BY j1.v DESC LIMIT 1;"))
    
    # 多表 JOIN
    run("CREATE TABLE j3 (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO j3 VALUES (1, 1000);", db)
    chk("3-way JOIN", "1000", runf(db, "SELECT j3.v FROM j1 INNER JOIN j2 ON j1.id = j2.id INNER JOIN j3 ON j2.id = j3.id;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 9. 表达式
# =================================================================
print("\n=== 9. 表达式 ===")
db = tempfile.mkdtemp()
try:
    # 算术
    chk("1+2*3=7", "7", runf(db, "SELECT 1 + 2 * 3;"))
    chk("(1+2)*3=9", "9", runf(db, "SELECT (1 + 2) * 3;"))
    chk("-1", "-1", runf(db, "SELECT -1;"))
    chk("10/3=3", "3", runf(db, "SELECT 10 / 3;"))
    chk("10%3=1", "1", runf(db, "SELECT 10 % 3;"))
    
    # 比较
    chk("1=1=1", "1", runf(db, "SELECT 1 = 1;"))
    chk("1=0=0", "0", runf(db, "SELECT 1 = 0;"))
    chk("1!=2=1", "1", runf(db, "SELECT 1 != 2;"))
    chk("1<>2=1", "1", runf(db, "SELECT 1 <> 2;"))
    
    # 布尔
    chk("TRUE AND TRUE=1", "1", runf(db, "SELECT TRUE AND TRUE;"))
    chk("TRUE AND FALSE=0", "0", runf(db, "SELECT TRUE AND FALSE;"))
    chk("TRUE OR FALSE=1", "1", runf(db, "SELECT TRUE OR FALSE;"))
    chk("NOT TRUE=0", "0", runf(db, "SELECT NOT TRUE;"))
    chk("NOT FALSE=1", "1", runf(db, "SELECT NOT FALSE;"))
    
    # NULL
    chk("1+NULL=NULL", "NULL", runf(db, "SELECT 1 + NULL;"))
    chk("NULL=NULL=NULL", "NULL", runf(db, "SELECT NULL = NULL;"))
    chk("NULL AND TRUE=NULL", "NULL", runf(db, "SELECT NULL AND TRUE;"))
    chk("NULL OR TRUE=1", "1", runf(db, "SELECT NULL OR TRUE;"))
    chk("NOT NULL=NULL", "NULL", runf(db, "SELECT NOT NULL;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 10. CASE / COALESCE / NULLIF
# =================================================================
print("\n=== 10. CASE / COALESCE / NULLIF ===")
db = tempfile.mkdtemp()
try:
    # CASE
    chk("CASE WHEN", "low", runf(db, "SELECT CASE WHEN 5 < 10 THEN 'low' ELSE 'high' END;"))
    chk("CASE multi", "mid", runf(db, "SELECT CASE WHEN 15 < 10 THEN 'low' WHEN 15 < 20 THEN 'mid' ELSE 'high' END;"))
    chk("CASE no ELSE", "NULL", runf(db, "SELECT CASE WHEN 100 < 10 THEN 'low' END;"))
    
    # COALESCE
    chk("COALESCE 2", "10", runf(db, "SELECT COALESCE(NULL, 10);"))
    chk("COALESCE 3", "20", runf(db, "SELECT COALESCE(NULL, NULL, 20);"))
    chk("COALESCE first", "10", runf(db, "SELECT COALESCE(10, 20);"))
    
    # NULLIF
    chk("NULLIF equal", "NULL", runf(db, "SELECT NULLIF(10, 10);"))
    chk("NULLIF diff", "10", runf(db, "SELECT NULLIF(10, 20);"))
finally:
    shutil.rmtree(db)

# =================================================================
# 11. 事务
# =================================================================
print("\n=== 11. 事务 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10);", db)
    
    # COMMIT
    run("BEGIN;", db)
    run("INSERT INTO t VALUES (2, 20);", db)
    run("COMMIT;", db)
    chk("COMMIT", "2", runf(db, "SELECT COUNT(*) FROM t;"))
    
    # ROLLBACK
    run("BEGIN;", db)
    run("INSERT INTO t VALUES (3, 30);", db)
    run("ROLLBACK;", db)
    chk("ROLLBACK", "2", runf(db, "SELECT COUNT(*) FROM t;"))
    
    # UPDATE + ROLLBACK
    run("BEGIN;", db)
    run("UPDATE t SET v = 99 WHERE id = 1;", db)
    run("ROLLBACK;", db)
    chk("UPDATE ROLLBACK", "10", runf(db, "SELECT v FROM t WHERE id = 1;"))
    
    # DELETE + ROLLBACK
    run("BEGIN;", db)
    run("DELETE FROM t WHERE id = 2;", db)
    run("ROLLBACK;", db)
    chk("DELETE ROLLBACK", "2", runf(db, "SELECT COUNT(*) FROM t;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 12. 子查询
# =================================================================
print("\n=== 12. 子查询 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE a (id INT PRIMARY KEY);", db)
    run("CREATE TABLE b (id INT PRIMARY KEY);", db)
    run("INSERT INTO a VALUES (1), (2), (3), (4), (5);", db)
    run("INSERT INTO b VALUES (2), (4);", db)
    
    chk("IN subquery", "2", runf(db, "SELECT id FROM a WHERE id IN (SELECT id FROM b);"))
    chk("NOT IN subquery", "1", runf(db, "SELECT id FROM a WHERE id NOT IN (SELECT id FROM b) LIMIT 1;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 13. ALTER TABLE
# =================================================================
print("\n=== 13. ALTER TABLE ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR);", db)
    run("INSERT INTO t VALUES (1, 'alice');", db)
    
    # ADD COLUMN
    run("ALTER TABLE t ADD COLUMN age INT DEFAULT 25;", db)
    chk("ADD COLUMN", "25", runf(db, "SELECT age FROM t;"))
    
    # RENAME COLUMN
    run("ALTER TABLE t RENAME COLUMN name TO full_name;", db)
    chk("RENAME", "full_name", runf(db, "DESC t;"))
    
    # DROP COLUMN
    run("ALTER TABLE t DROP COLUMN age;", db)
    chk("DROP COLUMN", "age", runf(db, "DESC t;"), negate=True)
finally:
    shutil.rmtree(db)

# =================================================================
# 14. 索引
# =================================================================
print("\n=== 14. 索引 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("CREATE INDEX idx_v ON t (v);", db)
    run("INSERT INTO t VALUES (1, 100), (2, 200), (3, 300);", db)
    
    chk("Index eq", "2", runf(db, "SELECT id FROM t WHERE v = 200;"))
    chk("Index range", "2", runf(db, "SELECT COUNT(*) FROM t WHERE v > 150 AND v < 250;"))
    
    # UPDATE + index
    run("UPDATE t SET v = 999 WHERE id = 1;", db)
    chk("Index after UPDATE", "1", runf(db, "SELECT id FROM t WHERE v = 999;"))
    
    # DELETE + index
    run("DELETE FROM t WHERE v = 999;", db)
    chk("Index after DELETE", "Goodbye", runf(db, "SELECT id FROM t WHERE v = 999;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 15. NULL 处理
# =================================================================
print("\n=== 15. NULL 处理 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT, s VARCHAR);", db)
    run("INSERT INTO t VALUES (1, NULL, NULL);", db)
    run("INSERT INTO t VALUES (2, 10, 'hello');", db)
    
    chk("IS NULL", "1", runf(db, "SELECT id FROM t WHERE v IS NULL;"))
    chk("IS NOT NULL", "2", runf(db, "SELECT id FROM t WHERE v IS NOT NULL;"))
    chk("v=NULL returns empty", "Goodbye", runf(db, "SELECT id FROM t WHERE v = NULL;"))
    chk("v!=NULL returns empty", "Goodbye", runf(db, "SELECT id FROM t WHERE v != NULL;"))
    
    # NULL in aggregates
    chk("COUNT(*) with NULL", "2", runf(db, "SELECT COUNT(*) FROM t;"))
    chk("COUNT(v) ignores NULL", "1", runf(db, "SELECT COUNT(v) FROM t;"))
    chk("SUM ignores NULL", "10", runf(db, "SELECT SUM(v) FROM t;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 16. 边界条件
# =================================================================
print("\n=== 16. 边界条件 ===")
db = tempfile.mkdtemp()
try:
    # 空表
    run("CREATE TABLE empty_t (id INT PRIMARY KEY, v INT);", db)
    chk("Empty COUNT", "0", runf(db, "SELECT COUNT(*) FROM empty_t;"))
    
    # 单行
    run("INSERT INTO empty_t VALUES (1, 42);", db)
    chk("Single row", "42", runf(db, "SELECT v FROM empty_t;"))
    
    # 负数
    run("CREATE TABLE neg (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO neg VALUES (1, -999);", db)
    chk("Negative value", "-999", runf(db, "SELECT v FROM neg;"))
    
    # 大数
    run("CREATE TABLE big (id INT PRIMARY KEY, v BIGINT);", db)
    run("INSERT INTO big VALUES (1, 9999999999);", db)
    chk("Big number", "9999999999", runf(db, "SELECT v FROM big;"))
    
    # 空字符串
    run("CREATE TABLE str (id INT PRIMARY KEY, s VARCHAR);", db)
    run("INSERT INTO str VALUES (1, '');", db)
    chk("Empty string", "Goodbye", runf(db, "SELECT s FROM str WHERE s = '';"))
    
    # 多次 DELETE + INSERT
    run("CREATE TABLE cycle (id INT PRIMARY KEY, v INT);", db)
    for i in range(1, 6):
        run(f"INSERT INTO cycle VALUES ({i}, {i});", db)
        run(f"DELETE FROM cycle WHERE id = {i};", db)
    chk("Delete cycle", "0", runf(db, "SELECT COUNT(*) FROM cycle;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 17. 多语句
# =================================================================
print("\n=== 17. 多语句 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 10); INSERT INTO t VALUES (2, 20);", db)
    chk("Multi INSERT", "2", runf(db, "SELECT COUNT(*) FROM t;"))
    
    run("UPDATE t SET v = v + 10 WHERE id = 1; UPDATE t SET v = v + 10 WHERE id = 2;", db)
    chk("Multi UPDATE", "30", runf(db, "SELECT SUM(v) FROM t;"))
    
    run("DELETE FROM t WHERE id = 1; DELETE FROM t WHERE id = 2;", db)
    chk("Multi DELETE", "0", runf(db, "SELECT COUNT(*) FROM t;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 18. 持久化
# =================================================================
print("\n=== 18. 持久化 ===")
db = tempfile.mkdtemp()
try:
    # INSERT 持久化
    run("CREATE TABLE p1 (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO p1 VALUES (1, 10), (2, 20);", db)
    chk("INSERT persist", "20", runf(db, "SELECT v FROM p1 WHERE id = 2;"))
    
    # UPDATE 持久化
    run("UPDATE p1 SET v = 99 WHERE id = 1;", db)
    chk("UPDATE persist", "99", runf(db, "SELECT v FROM p1 WHERE id = 1;"))
    
    # DELETE 持久化
    run("DELETE FROM p1 WHERE id = 2;", db)
    chk("DELETE persist", "1", runf(db, "SELECT COUNT(*) FROM p1;"))
    
    # ALTER TABLE 持久化
    run("CREATE TABLE p2 (id INT PRIMARY KEY, name VARCHAR);", db)
    run("INSERT INTO p2 VALUES (1, 'alice');", db)
    run("ALTER TABLE p2 ADD COLUMN age INT DEFAULT 25;", db)
    chk("ALTER persist", "25", runf(db, "SELECT age FROM p2;"))
    
    # INDEX 持久化
    run("CREATE TABLE p3 (id INT PRIMARY KEY, v INT);", db)
    run("CREATE INDEX p3v ON p3 (v);", db)
    run("INSERT INTO p3 VALUES (1, 100);", db)
    chk("INDEX persist", "1", runf(db, "SELECT id FROM p3 WHERE v = 100;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 19. UPDATE 连续操作
# =================================================================
print("\n=== 19. UPDATE 连续 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE t (id INT PRIMARY KEY, v INT);", db)
    run("INSERT INTO t VALUES (1, 0);", db)
    
    for _ in range(10):
        run("UPDATE t SET v = v + 1;", db)
    chk("10 UPDATEs", "10", runf(db, "SELECT v FROM t;"))
    
    run("UPDATE t SET v = v * 2;", db)
    chk("20 after *2", "20", runf(db, "SELECT v FROM t;"))
    
    run("UPDATE t SET v = v - 5;", db)
    chk("15 after -5", "15", runf(db, "SELECT v FROM t;"))
finally:
    shutil.rmtree(db)

# =================================================================
# 20. JOIN + 聚合
# =================================================================
print("\n=== 20. JOIN + 聚合 ===")
db = tempfile.mkdtemp()
try:
    run("CREATE TABLE orders (id INT PRIMARY KEY, cid INT, amt INT);", db)
    run("CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR);", db)
    run("INSERT INTO customers VALUES (1, 'Alice'), (2, 'Bob');", db)
    run("INSERT INTO orders VALUES (1, 1, 100), (2, 1, 200), (3, 2, 300);", db)
    
    chk("JOIN+COUNT", "2", runf(db, "SELECT COUNT(*) FROM customers INNER JOIN orders ON customers.id = orders.cid WHERE customers.id = 1;"))
    chk("JOIN+SUM", "300", runf(db, "SELECT SUM(orders.amt) FROM customers INNER JOIN orders ON customers.id = orders.cid WHERE customers.id = 1;"))
finally:
    shutil.rmtree(db)

# =================================================================
print("\n" + "=" * 50)
print(f"  RESULTS: {PASS}/{PASS+FAIL} passed, {FAIL} failed")
print("=" * 50)
if BUGS:
    print("\n--- BUGS ---")
    for desc, needle, got in BUGS:
        print(f"  {desc} (expected '{needle}', got '{got[:100]}')")
    sys.exit(1)
else:
    print("\nAll combination tests passed!")
    sys.exit(0)
