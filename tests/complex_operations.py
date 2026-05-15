#!/usr/bin/env python3
"""
复杂操作正确性测试 — 覆盖多步骤、多表、边界场景
"""
import subprocess, os, sys, tempfile, shutil

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
PASS = FAIL = 0
BUGS = []

class DB:
    def __init__(self):
        self.d = tempfile.mkdtemp(prefix="cx_")
    def rm(self): shutil.rmtree(self.d, ignore_errors=True)
    def run(self, sqls):
        if isinstance(sqls, str): sqls = [sqls]
        sqls.append("exit")
        r = subprocess.run([BIN, "--dir", self.d], input="\n".join(sqls),
                           capture_output=True, text=True, timeout=60)
        return r.stdout + r.stderr
    def extract(self, out):
        lines = []
        for l in out.split('\n'):
            s = l.strip()
            if s.startswith('minidb> '): s = s[8:]
            if s and not s.startswith('Data dir') and not s.startswith('MiniADB') \
               and not s.startswith('Type ') and not s.startswith('[DB]') \
               and s != 'Goodbye.' and s != 'Tables:':
                lines.append(s)
        return '\n'.join(lines)

def chk(desc, needle, out, negate=False):
    global PASS, FAIL
    clean = DB().extract(out) if isinstance(out, str) else out
    found = needle in clean
    if negate: found = not found
    if found: PASS += 1
    else: FAIL += 1; BUGS.append((desc, needle, clean[:200])); print(f"  FAIL: {desc}")

# ================================================================
# 1. 多表关联查询
# ================================================================
print("=== 1. 多表关联查询 ===")
db = DB()
try:
    db.run(["CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR);",
            "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, total INT);",
            "INSERT INTO customers VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie');",
            "INSERT INTO orders VALUES (1, 1, 100), (2, 1, 200), (3, 2, 150);"])
    chk("JOIN: Alice orders=2", "2", db.run(
        "SELECT COUNT(*) FROM customers c INNER JOIN orders o ON c.id = o.customer_id WHERE c.name = 'Alice';"))
    chk("JOIN: Bob orders=1", "1", db.run(
        "SELECT COUNT(*) FROM customers c INNER JOIN orders o ON c.id = o.customer_id WHERE c.name = 'Bob';"))
    chk("LEFT JOIN: Charlie has 0 orders", "3", db.run(
        "SELECT c.id FROM customers c LEFT JOIN orders o ON c.id = o.customer_id WHERE o.id IS NULL;"))
finally: db.rm()

# ================================================================
# 2. 电商场景: 多表 + 聚合 + JOIN
# ================================================================
print("\n=== 2. 电商场景 ===")
db = DB()
try:
    db.run(["CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR, price DOUBLE);",
            "CREATE TABLE order_items (id INT PRIMARY KEY, order_id INT, product_id INT, qty INT);",
            "INSERT INTO products VALUES (1, 'Laptop', 999.99), (2, 'Mouse', 29.99), (3, 'Keyboard', 79.99);",
            "INSERT INTO order_items VALUES (1, 1, 1, 2), (2, 1, 2, 3), (3, 2, 3, 1);"])
    chk("Product count=3", "3", db.run("SELECT COUNT(*) FROM products;"))
    chk("Order items count=3", "3", db.run("SELECT COUNT(*) FROM order_items;"))
    chk("Items in order 1: qty sum=5", "5", db.run("SELECT SUM(qty) FROM order_items WHERE order_id = 1;"))
finally: db.rm()

# ================================================================
# 3. 分页查询
# ================================================================
print("\n=== 3. 分页查询 ===")
db = DB()
try:
    sqls = ["CREATE TABLE pages (id INT PRIMARY KEY, v INT);"]
    for i in range(1, 21): sqls.append(f"INSERT INTO pages VALUES ({i}, {i*10});")
    db.run(sqls)
    chk("Page 1: first=1", "1", db.run("SELECT id FROM pages ORDER BY id ASC LIMIT 5 OFFSET 0;"))
    chk("Page 2: first=6", "6", db.run("SELECT id FROM pages ORDER BY id ASC LIMIT 5 OFFSET 5;"))
    chk("Page 3: first=11", "11", db.run("SELECT id FROM pages ORDER BY id ASC LIMIT 5 OFFSET 10;"))
finally: db.rm()

# ================================================================
# 4. 级联 DELETE + 索引维护
# ================================================================
print("\n=== 4. 级联 DELETE + 索引 ===")
db = DB()
try:
    db.run(["CREATE TABLE cd_t (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX cd_idx ON cd_t (v);",
            "INSERT INTO cd_t VALUES (1, 10), (2, 20), (3, 30), (4, 40);"])
    db.run("DELETE FROM cd_t WHERE v > 20;")
    chk("DELETE >20: count=2", "2", db.run("SELECT COUNT(*) FROM cd_t;"))
    chk("Index: v=30 gone", "30", db.run("SELECT v FROM cd_t WHERE v = 30;"), negate=True)
    chk("Index: v=10 exists", "10", db.run("SELECT v FROM cd_t WHERE v = 10;"))
finally: db.rm()

# ================================================================
# 5. UPDATE + 索引维护
# ================================================================
print("\n=== 5. UPDATE + 索引维护 ===")
db = DB()
try:
    db.run(["CREATE TABLE ui_t (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX ui_idx ON ui_t (v);",
            "INSERT INTO ui_t VALUES (1, 100), (2, 200);"])
    db.run("UPDATE ui_t SET v = 999 WHERE id = 1;")
    chk("UPDATE index: v=999", "1", db.run("SELECT id FROM ui_t WHERE v = 999;"))
    chk("UPDATE index: v=100 gone", "100", db.run("SELECT v FROM ui_t WHERE v = 100;"), negate=True)
finally: db.rm()

# ================================================================
# 6. 批量 INSERT + 批量 DELETE + 重新 INSERT
# ================================================================
print("\n=== 6. 批量操作 ===")
db = DB()
try:
    sqls = ["CREATE TABLE batch (id INT PRIMARY KEY, v INT);"]
    for i in range(1, 101): sqls.append(f"INSERT INTO batch VALUES ({i}, {i});")
    sqls.append("DELETE FROM batch;")
    for i in range(1, 51): sqls.append(f"INSERT INTO batch VALUES ({i}, {i*10});")
    db.run(sqls)
    chk("Batch: count=50", "50", db.run("SELECT COUNT(*) FROM batch;"))
    chk("Batch: sum=12750", "12750", db.run("SELECT SUM(v) FROM batch;"))
finally: db.rm()

# ================================================================
# 7. 事务隔离: COMMIT 后可见
# ================================================================
print("\n=== 7. 事务隔离 ===")
db = DB()
try:
    db.run(["CREATE TABLE iso (id INT PRIMARY KEY, v INT);",
            "INSERT INTO iso VALUES (1, 10);",
            "BEGIN;", "UPDATE iso SET v = 99 WHERE id = 1;", "COMMIT;"])
    chk("COMMIT visible: v=99", "99", db.run("SELECT v FROM iso;"))
finally: db.rm()

# ================================================================
# 8. 事务回滚: UPDATE ROLLBACK
# ================================================================
print("\n=== 8. UPDATE ROLLBACK ===")
db = DB()
try:
    db.run(["CREATE TABLE urb (id INT PRIMARY KEY, v INT);",
            "INSERT INTO urb VALUES (1, 100);",
            "BEGIN;", "UPDATE urb SET v = 999;", "ROLLBACK;"])
    chk("ROLLBACK: v=100", "100", db.run("SELECT v FROM urb;"))
finally: db.rm()

# ================================================================
# 9. 事务回滚: DELETE ROLLBACK
# ================================================================
print("\n=== 9. DELETE ROLLBACK ===")
db = DB()
try:
    db.run(["CREATE TABLE dbr (id INT PRIMARY KEY, v INT);",
            "INSERT INTO dbr VALUES (1, 10), (2, 20);",
            "BEGIN;", "DELETE FROM dbr WHERE id = 1;", "ROLLBACK;"])
    chk("ROLLBACK DELETE: count=2", "2", db.run("SELECT COUNT(*) FROM dbr;"))
finally: db.rm()

# ================================================================
# 10. 多事务交替
# ================================================================
print("\n=== 10. 多事务交替 ===")
db = DB()
try:
    db.run(["CREATE TABLE alt_txn (id INT PRIMARY KEY, v INT);",
            "INSERT INTO alt_txn VALUES (1, 10);",
            "BEGIN;", "INSERT INTO alt_txn VALUES (2, 20);", "COMMIT;",
            "BEGIN;", "INSERT INTO alt_txn VALUES (3, 30);", "ROLLBACK;",
            "BEGIN;", "INSERT INTO alt_txn VALUES (4, 40);", "COMMIT;"])
    chk("交替: count=3", "3", db.run("SELECT COUNT(*) FROM alt_txn;"))
finally: db.rm()

# ================================================================
# 11. 空表 + 聚合 + 重启
# ================================================================
print("\n=== 11. 空表聚合 ===")
db = DB()
try:
    db.run("CREATE TABLE empty_agg (v INT);")
    chk("Empty COUNT=0", "0", db.run("SELECT COUNT(*) FROM empty_agg;"))
    chk("Empty SUM=NULL", "NULL", db.run("SELECT SUM(v) FROM empty_agg;"))
    chk("Empty AVG=NULL", "NULL", db.run("SELECT AVG(v) FROM empty_agg;"))
finally: db.rm()

# ================================================================
# 12. 大量 NULL + 聚合
# ================================================================
print("\n=== 12. NULL 聚合 ===")
db = DB()
try:
    db.run(["CREATE TABLE null_agg (v INT);",
            "INSERT INTO null_agg VALUES (NULL), (NULL), (10), (NULL), (30);"])
    chk("COUNT(*): 5", "5", db.run("SELECT COUNT(*) FROM null_agg;"))
    chk("COUNT(v): 2", "2", db.run("SELECT COUNT(v) FROM null_agg;"))
    chk("SUM: 40", "40", db.run("SELECT SUM(v) FROM null_agg;"))
    chk("AVG: 20", "20", db.run("SELECT AVG(v) FROM null_agg;"))
finally: db.rm()

# ================================================================
# 13. JOIN + WHERE + ORDER BY
# ================================================================
print("\n=== 13. JOIN + WHERE + ORDER BY ===")
db = DB()
try:
    db.run(["CREATE TABLE jw1 (id INT PRIMARY KEY, v INT);",
            "CREATE TABLE jw2 (id INT PRIMARY KEY, v INT);",
            "INSERT INTO jw1 VALUES (1, 10), (2, 20), (3, 30);",
            "INSERT INTO jw2 VALUES (1, 100), (2, 200), (4, 400);"])
    chk("JOIN+WHERE: jw1.v>10", "2", db.run(
        "SELECT jw1.id FROM jw1 INNER JOIN jw2 ON jw1.id = jw2.id WHERE jw1.v > 10 ORDER BY jw1.id;"))
finally: db.rm()

# ================================================================
# 14. GROUP BY + COUNT + SUM
# ================================================================
print("\n=== 14. GROUP BY 聚合 ===")
db = DB()
try:
    db.run(["CREATE TABLE gba (cat VARCHAR, v INT);",
            "INSERT INTO gba VALUES ('A', 10), ('A', 20), ('B', 30), ('B', 40), ('B', 50);"])
    chk("GROUP A: count=2", "2", db.run("SELECT COUNT(*) FROM gba WHERE cat = 'A';"))
    chk("GROUP B: sum=120", "120", db.run("SELECT SUM(v) FROM gba WHERE cat = 'B';"))
finally: db.rm()

# ================================================================
# 15. DISTINCT + ORDER BY
# ================================================================
print("\n=== 15. DISTINCT + ORDER BY ===")
db = DB()
try:
    db.run(["CREATE TABLE do_t (v INT);",
            "INSERT INTO do_t VALUES (3), (1), (2), (1), (3), (2);"])
    chk("DISTINCT count=3", "3", db.run("SELECT COUNT(DISTINCT v) FROM do_t;"))
finally: db.rm()

# ================================================================
# 16. 多列 UNIQUE 约束
# ================================================================
print("\n=== 16. UNIQUE 约束 ===")
db = DB()
try:
    db.run(["CREATE TABLE uniq_t (id INT PRIMARY KEY, email VARCHAR UNIQUE);",
            "INSERT INTO uniq_t VALUES (1, 'a@b.com');",
            "INSERT INTO uniq_t VALUES (2, 'a@b.com');",
            "INSERT INTO uniq_t VALUES (3, 'c@d.com');"])
    chk("UNIQUE: only 2 rows", "2", db.run("SELECT COUNT(*) FROM uniq_t;"))
finally: db.rm()

# ================================================================
# 17. NOT NULL 约束
# ================================================================
print("\n=== 17. NOT NULL 约束 ===")
db = DB()
try:
    db.run(["CREATE TABLE nn_t (id INT PRIMARY KEY, v INT NOT NULL);",
            "INSERT INTO nn_t VALUES (1, 10);",
            "INSERT INTO nn_t VALUES (2, NULL);"])
    chk("NOT NULL: only 1 row", "1", db.run("SELECT COUNT(*) FROM nn_t;"))
finally: db.rm()

# ================================================================
# 18. 复合主键 (通过 UNIQUE 索引)
# ================================================================
print("\n=== 18. 复合索引 ===")
db = DB()
try:
    db.run(["CREATE TABLE comp (a INT, b INT, v INT);",
            "CREATE INDEX comp_ab ON comp (a, b);",
            "INSERT INTO comp VALUES (1, 1, 100), (1, 2, 200), (2, 1, 300);"])
    chk("Composite: a=1,b=2→200", "200", db.run("SELECT v FROM comp WHERE a = 1 AND b = 2;"))
    chk("Composite: a=2,b=1→300", "300", db.run("SELECT v FROM comp WHERE a = 2 AND b = 1;"))
finally: db.rm()

# ================================================================
# 19. UPDATE 全表 + 索引
# ================================================================
print("\n=== 19. UPDATE 全表 + 索引 ===")
db = DB()
try:
    db.run(["CREATE TABLE ua_t (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX ua_idx ON ua_t (v);",
            "INSERT INTO ua_t VALUES (1, 10), (2, 20), (3, 30);"])
    db.run("UPDATE ua_t SET v = v + 100;")
    chk("UPDATE all: v1=110", "110", db.run("SELECT v FROM ua_t WHERE id = 1;"))
    chk("UPDATE all: v2=120", "120", db.run("SELECT v FROM ua_t WHERE id = 2;"))
    chk("Index: v=110", "1", db.run("SELECT id FROM ua_t WHERE v = 110;"))
finally: db.rm()

# ================================================================
# 20. DELETE + INSERT 同表循环
# ================================================================
print("\n=== 20. DELETE + INSERT 循环 ===")
db = DB()
try:
    db.run("CREATE TABLE cycle (id INT PRIMARY KEY, v INT);")
    for i in range(1, 6):
        db.run(f"INSERT INTO cycle VALUES ({i}, {i*10});")
        db.run(f"DELETE FROM cycle WHERE id = {i};")
    chk("Cycle: count=0", "0", db.run("SELECT COUNT(*) FROM cycle;"))
finally: db.rm()

# ================================================================
# 21. 大量 UPDATE 同一行
# ================================================================
print("\n=== 21. 大量 UPDATE 同一行 ===")
db = DB()
try:
    db.run(["CREATE TABLE multi_upd (id INT PRIMARY KEY, v INT);",
            "INSERT INTO multi_upd VALUES (1, 0);"])
    for _ in range(20):
        db.run("UPDATE multi_upd SET v = v + 1;")
    chk("20 UPDATEs: v=20", "20", db.run("SELECT v FROM multi_upd;"))
finally: db.rm()

# ================================================================
# 22. 多表 DELETE + 索引
# ================================================================
print("\n=== 22. 多表 DELETE + 索引 ===")
db = DB()
try:
    db.run(["CREATE TABLE md1 (id INT PRIMARY KEY, v INT);",
            "CREATE TABLE md2 (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX md1_idx ON md1 (v);",
            "INSERT INTO md1 VALUES (1, 10), (2, 20);",
            "INSERT INTO md2 VALUES (1, 100);"])
    db.run("DELETE FROM md1 WHERE v = 10;")
    chk("MD1: count=1", "1", db.run("SELECT COUNT(*) FROM md1;"))
    chk("MD2: unchanged", "1", db.run("SELECT COUNT(*) FROM md2;"))
finally: db.rm()

# ================================================================
# 23. UPDATE SET NULL
# ================================================================
print("\n=== 23. UPDATE SET NULL ===")
db = DB()
try:
    db.run(["CREATE TABLE usn (id INT PRIMARY KEY, v INT);",
            "INSERT INTO usn VALUES (1, 10);"])
    db.run("UPDATE usn SET v = NULL;")
    chk("SET NULL: v=NULL", "NULL", db.run("SELECT v FROM usn;"))
finally: db.rm()

# ================================================================
# 24. INSERT + UPDATE + DELETE 全流程
# ================================================================
print("\n=== 24. 全流程 ===")
db = DB()
try:
    db.run(["CREATE TABLE full_flow (id INT PRIMARY KEY, v INT);",
            "INSERT INTO full_flow VALUES (1, 10), (2, 20), (3, 30);",
            "UPDATE full_flow SET v = 99 WHERE id = 2;",
            "DELETE FROM full_flow WHERE id = 3;",
            "INSERT INTO full_flow VALUES (4, 40);"])
    chk("Full flow: count=3", "3", db.run("SELECT COUNT(*) FROM full_flow;"))
    chk("Full flow: v2=99", "99", db.run("SELECT v FROM full_flow WHERE id = 2;"))
    chk("Full flow: id=4", "4", db.run("SELECT id FROM full_flow WHERE id = 4;"))
    chk("Full flow: id=3 gone", "3", db.run("SELECT id FROM full_flow WHERE id = 3;"), negate=True)
finally: db.rm()

# ================================================================
# 25. 索引 + 事务
# ================================================================
print("\n=== 25. 索引 + 事务 ===")
db = DB()
try:
    db.run(["CREATE TABLE it_t (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX it_idx ON it_t (v);",
            "INSERT INTO it_t VALUES (1, 100);",
            "BEGIN;", "INSERT INTO it_t VALUES (2, 200);", "COMMIT;"])
    chk("Index+txn: v=200", "2", db.run("SELECT id FROM it_t WHERE v = 200;"))
finally: db.rm()

# ================================================================
# 26. 索引 + ROLLBACK
# ================================================================
print("\n=== 26. 索引 + ROLLBACK ===")
db = DB()
try:
    db.run(["CREATE TABLE ir_t (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX ir_idx ON ir_t (v);",
            "INSERT INTO ir_t VALUES (1, 100);",
            "BEGIN;", "INSERT INTO ir_t VALUES (2, 200);", "ROLLBACK;"])
    chk("Index+rollback: v=200 gone", "200", db.run("SELECT v FROM ir_t WHERE v = 200;"), negate=True)
finally: db.rm()

# ================================================================
# 27. DROP INDEX + 查询回退
# ================================================================
print("\n=== 27. DROP INDEX ===")
db = DB()
try:
    db.run(["CREATE TABLE di_t (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX di_idx ON di_t (v);",
            "INSERT INTO di_t VALUES (1, 100);"])
    chk("Before DROP: indexed query", "1", db.run("SELECT id FROM di_t WHERE v = 100;"))
    db.run("DROP INDEX di_idx;")
    chk("After DROP: still queryable", "1", db.run("SELECT id FROM di_t WHERE v = 100;"))
finally: db.rm()

# ================================================================
# 28. 多列索引范围查询
# ================================================================
print("\n=== 28. 复合索引范围 ===")
db = DB()
try:
    db.run(["CREATE TABLE cir (a INT, b INT, v INT);",
            "CREATE INDEX cir_idx ON cir (a);",
            "INSERT INTO cir VALUES (1, 10, 100), (1, 20, 200), (2, 10, 300);"])
    chk("Index range: a=1→2 rows", "2", db.run("SELECT COUNT(*) FROM cir WHERE a = 1;"))
finally: db.rm()

# ================================================================
# 29. UPDATE 主键
# ================================================================
print("\n=== 29. UPDATE 主键 ===")
db = DB()
try:
    db.run(["CREATE TABLE upk (id INT PRIMARY KEY, v INT);",
            "INSERT INTO upk VALUES (1, 10);"])
    db.run("UPDATE upk SET id = 99 WHERE id = 1;")
    chk("UPDATE PK: id=99", "99", db.run("SELECT id FROM upk;"))
    chk("UPDATE PK: id=1 gone", "1", db.run("SELECT id FROM upk WHERE id = 1;"), negate=True)
finally: db.rm()

# ================================================================
# 30. 大量索引操作
# ================================================================
print("\n=== 30. 大量索引操作 ===")
db = DB()
try:
    sqls = ["CREATE TABLE bulk_idx (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX bulk_v ON bulk_idx (v);"]
    for i in range(1, 101): sqls.append(f"INSERT INTO bulk_idx VALUES ({i}, {i % 10});")
    db.run(sqls)
    chk("Bulk idx: v=5 count=10", "10", db.run("SELECT COUNT(*) FROM bulk_idx WHERE v = 5;"))
    chk("Bulk idx: v=0 count=10", "10", db.run("SELECT COUNT(*) FROM bulk_idx WHERE v = 0;"))
finally: db.rm()

# ================================================================
# 31. SELECT 子查询 IN
# ================================================================
print("\n=== 31. 子查询 IN ===")
db = DB()
try:
    db.run(["CREATE TABLE sq_a (id INT PRIMARY KEY);", "CREATE TABLE sq_b (id INT PRIMARY KEY);",
            "INSERT INTO sq_a VALUES (1), (2), (3), (4), (5);",
            "INSERT INTO sq_b VALUES (2), (4);"])
    chk("Subquery IN: 2,4", "2", db.run("SELECT id FROM sq_a WHERE id IN (SELECT id FROM sq_b) ORDER BY id;"))
finally: db.rm()

# ================================================================
# 32. UNION ALL
# ================================================================
print("\n=== 32. UNION ALL ===")
db = DB()
try:
    db.run(["CREATE TABLE ua_l (id INT);", "CREATE TABLE ua_r (id INT);",
            "INSERT INTO ua_l VALUES (1), (2);", "INSERT INTO ua_r VALUES (2), (3);"])
    chk("UNION ALL: 1,2,2,3", "1", db.run("SELECT id FROM ua_l UNION ALL SELECT id FROM ua_r;"))
finally: db.rm()

# ================================================================
# 33. CASE WHEN 多分支
# ================================================================
print("\n=== 33. CASE 多分支 ===")
db = DB()
try:
    db.run(["CREATE TABLE cmb (id INT PRIMARY KEY, v INT);",
            "INSERT INTO cmb VALUES (1, 5), (2, 15), (3, 25);"])
    chk("CASE low", "low", db.run("SELECT CASE WHEN v < 10 THEN 'low' WHEN v < 20 THEN 'mid' ELSE 'high' END FROM cmb WHERE id = 1;"))
    chk("CASE mid", "mid", db.run("SELECT CASE WHEN v < 10 THEN 'low' WHEN v < 20 THEN 'mid' ELSE 'high' END FROM cmb WHERE id = 2;"))
    chk("CASE high", "high", db.run("SELECT CASE WHEN v < 10 THEN 'low' WHEN v < 20 THEN 'mid' ELSE 'high' END FROM cmb WHERE id = 3;"))
finally: db.rm()

# ================================================================
# 34. COALESCE 多参数
# ================================================================
print("\n=== 34. COALESCE 多参数 ===")
db = DB()
try:
    db.run("CREATE TABLE coal3 (a INT, b INT, c INT); INSERT INTO coal3 VALUES (NULL, NULL, 30);")
    chk("COALESCE(NULL,NULL,30)=30", "30", db.run("SELECT COALESCE(a, b, c) FROM coal3;"))
finally: db.rm()

# ================================================================
# 35. 算术优先级
# ================================================================
print("\n=== 35. 算术优先级 ===")
db = DB()
try:
    chk("1+2*3=7", "7", db.run("SELECT 1 + 2 * 3;"))
    chk("(1+2)*3=9", "9", db.run("SELECT (1 + 2) * 3;"))
finally: db.rm()

# ================================================================
# 36. 比较 + 逻辑组合
# ================================================================
print("\n=== 36. 比较 + 逻辑组合 ===")
db = DB()
try:
    db.run(["CREATE TABLE cmp_logic (id INT PRIMARY KEY, a INT, b INT);",
            "INSERT INTO cmp_logic VALUES (1, 10, 20), (2, 30, 40);"])
    chk("(a>5 AND b<25)→1", "1", db.run("SELECT id FROM cmp_logic WHERE a > 5 AND b < 25;"))
    chk("(a=10 OR b=40)→1,2", "1", db.run("SELECT id FROM cmp_logic WHERE a = 10 OR b = 40 ORDER BY id;"))
finally: db.rm()

# ================================================================
# 37. 持久化: ALTER TABLE + 重启
# ================================================================
print("\n=== 37. ALTER TABLE 持久化 ===")
db = DB()
try:
    db.run(["CREATE TABLE alt_p (id INT PRIMARY KEY, name VARCHAR);",
            "INSERT INTO alt_p VALUES (1, 'alice');",
            "ALTER TABLE alt_p ADD COLUMN age INT DEFAULT 25;"])
    chk("ALTER持久化: age=25", "25", db.run("SELECT age FROM alt_p;"))
    chk("ALTER持久化: name preserved", "alice", db.run("SELECT name FROM alt_p;"))
finally: db.rm()

# ================================================================
# 38. 持久化: 索引 + 重启
# ================================================================
print("\n=== 38. 索引持久化 ===")
db = DB()
try:
    db.run(["CREATE TABLE idx_p (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX idx_p_v ON idx_p (v);",
            "INSERT INTO idx_p VALUES (1, 100), (2, 200);"])
    chk("索引持久化: v=100", "1", db.run("SELECT id FROM idx_p WHERE v = 100;"))
    chk("索引持久化: v=200", "2", db.run("SELECT id FROM idx_p WHERE v = 200;"))
finally: db.rm()

# ================================================================
# 39. 持久化: DROP TABLE + 重启
# ================================================================
print("\n=== 39. DROP TABLE 持久化 ===")
db = DB()
try:
    db.run(["CREATE TABLE drop_p (id INT PRIMARY KEY);",
            "INSERT INTO drop_p VALUES (1);",
            "DROP TABLE drop_p;"])
    chk("DROP持久化: gone", "Error", db.run("SELECT * FROM drop_p;"))
finally: db.rm()

# ================================================================
# 40. 持久化: 大量数据 + 重启
# ================================================================
print("\n=== 40. 大量数据持久化 ===")
db = DB()
try:
    sqls = ["CREATE TABLE big_p (id INT PRIMARY KEY, v INT);"]
    for i in range(1, 501): sqls.append(f"INSERT INTO big_p VALUES ({i}, {i*3});")
    db.run(sqls)
    chk("Big persist: count=500", "500", db.run("SELECT COUNT(*) FROM big_p;"))
    chk("Big persist: sum=375750", "375750", db.run("SELECT SUM(v) FROM big_p;"))
finally: db.rm()

# ================================================================
print("\n" + "=" * 50)
print(f"  COMPLEX OPERATIONS RESULTS: {PASS}/{PASS+FAIL} passed, {FAIL} failed")
print("=" * 50)
if BUGS:
    print("\n--- DISCOVERED BUGS ---")
    for d, e, g in BUGS:
        print(f"\n[BUG] {d}\n  Expected: {e}\n  Got: {g}")
    sys.exit(1)
else:
    print("\nAll complex operation tests passed!")
    sys.exit(0)
