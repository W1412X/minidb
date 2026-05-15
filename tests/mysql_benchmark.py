#!/usr/bin/env python3
"""MySQL vs MiniDB Benchmark — 1000 rows each"""
import subprocess, time, statistics, tempfile, shutil, sys

MINIDB = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
ITERATIONS = 3

def time_mysql(sql):
    times = []
    for _ in range(ITERATIONS):
        start = time.time()
        subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e", sql],
                       capture_output=True, text=True, timeout=60)
        times.append(time.time() - start)
    return min(times), statistics.mean(times), max(times)

def time_minidb(sqls, db_dir):
    times = []
    for _ in range(ITERATIONS):
        sqls.append("exit")
        start = time.time()
        subprocess.run([MINIDB, "--dir", db_dir], input="\n".join(sqls),
                       capture_output=True, text=True, timeout=120)
        times.append(time.time() - start)
        sqls.pop()
    return min(times), statistics.mean(times), max(times)

def p(op, mt, md):
    r = mt/md if md > 0 else float('inf')
    w = "MiniDB" if md < mt else "MySQL"
    print(f"  {op:35s}: MySQL {mt:.3f}s | MiniDB {md:.3f}s | {w} {r:.1f}x")

print("=" * 75)
print("  MySQL vs MiniDB Benchmark (1000 rows)")
print("=" * 75)

# MySQL setup
subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e",
                 "DROP DATABASE IF EXISTS bench; CREATE DATABASE bench"],
                shell=False, capture_output=True, text=True, timeout=30)
prefix = "USE bench; "

# MiniDB setup
mdb = tempfile.mkdtemp(prefix="bench_")
subprocess.run([MINIDB, "--dir", mdb], input="\n".join([
    "CREATE TABLE t (id INT PRIMARY KEY, v INT, s VARCHAR);",
    "CREATE INDEX idx_v ON t (v);",
    "exit"]),
    capture_output=True, text=True, timeout=30)

# 1. INSERT 1000
print("\n[INSERT]")
t1 = time_mysql(prefix + "; ".join([f"INSERT INTO t VALUES ({i}, {i}, 's{i}')" for i in range(1, 1001)]))
t2 = time_minidb([f"INSERT INTO t VALUES ({i}, {i}, 's{i}');" for i in range(1, 1001)], mdb)
p("INSERT 1000 rows", t1[1], t2[1])

# 2. SELECT point
print("\n[SELECT]")
t1 = time_mysql(prefix + "; ".join([f"SELECT v FROM t WHERE id = {i}" for i in range(1, 101)]))
t2 = time_minidb([f"SELECT v FROM t WHERE id = {i};" for i in range(1, 101)], mdb)
p("SELECT 100 point queries", t1[1], t2[1])

t1 = time_mysql(prefix + "SELECT COUNT(*) FROM t")
t2 = time_minidb(["SELECT COUNT(*) FROM t;"], mdb)
p("SELECT COUNT(*)", t1[1], t2[1])

t1 = time_mysql(prefix + "SELECT * FROM t WHERE v > 500")
t2 = time_minidb(["SELECT * FROM t WHERE v > 500;"], mdb)
p("SELECT WHERE v > 500", t1[1], t2[1])

# 3. UPDATE
print("\n[UPDATE]")
t1 = time_mysql(prefix + "; ".join([f"UPDATE t SET v = v + 1 WHERE id = {i}" for i in range(1, 101)]))
t2 = time_minidb([f"UPDATE t SET v = v + 1 WHERE id = {i};" for i in range(1, 101)], mdb)
p("UPDATE 100 rows", t1[1], t2[1])

t1 = time_mysql(prefix + "UPDATE t SET v = v * 2")
t2 = time_minidb(["UPDATE t SET v = v * 2;"], mdb)
p("UPDATE all rows", t1[1], t2[1])

# 4. DELETE
print("\n[DELETE]")
t1 = time_mysql(prefix + "; ".join([f"DELETE FROM t WHERE id = {i}" for i in range(1, 101)]))
t2 = time_minidb([f"DELETE FROM t WHERE id = {i};" for i in range(1, 101)], mdb)
p("DELETE 100 rows", t1[1], t2[1])

# 5. Aggregates
print("\n[AGGREGATE]")
t1 = time_mysql(prefix + "SELECT COUNT(*), SUM(v), AVG(v), MIN(v), MAX(v) FROM t")
t2 = time_minidb(["SELECT COUNT(*), SUM(v), AVG(v), MIN(v), MAX(v) FROM t;"], mdb)
p("5 aggregates", t1[1], t2[1])

t1 = time_mysql(prefix + "SELECT v/10, COUNT(*) FROM t GROUP BY v/10 HAVING COUNT(*) > 1")
t2 = time_minidb(["SELECT v/10, COUNT(*) FROM t GROUP BY v/10 HAVING COUNT(*) > 1;"], mdb)
p("GROUP BY + HAVING", t1[1], t2[1])

# 6. ORDER BY
print("\n[ORDER BY]")
t1 = time_mysql(prefix + "SELECT * FROM t ORDER BY v DESC LIMIT 10")
t2 = time_minidb(["SELECT * FROM t ORDER BY v DESC LIMIT 10;"], mdb)
p("ORDER BY + LIMIT", t1[1], t2[1])

t1 = time_mysql(prefix + "SELECT DISTINCT v/10 FROM t")
t2 = time_minidb(["SELECT DISTINCT v/10 FROM t;"], mdb)
p("DISTINCT", t1[1], t2[1])

# 7. Subquery
print("\n[SUBQUERY]")
t1 = time_mysql(prefix + "SELECT * FROM t WHERE id IN (SELECT id FROM t WHERE v > 500)")
t2 = time_minidb(["SELECT * FROM t WHERE id IN (SELECT id FROM t WHERE v > 500);"], mdb)
p("IN subquery", t1[1], t2[1])

t1 = time_mysql(prefix + "SELECT * FROM t WHERE id NOT IN (SELECT id FROM t WHERE v < 100)")
t2 = time_minidb(["SELECT * FROM t WHERE id NOT IN (SELECT id FROM t WHERE v < 100);"], mdb)
p("NOT IN subquery", t1[1], t2[1])

# 8. Transaction
print("\n[TRANSACTION]")
t1 = time_mysql(prefix + "; ".join(["BEGIN;"] * 10 + ["INSERT INTO t VALUES (1, 10, 'x');"] * 10 + ["COMMIT;"] * 10))
t2 = time_minidb(["BEGIN;", "INSERT INTO t VALUES (1, 10, 'x');", "COMMIT;"] * 10, mdb)
p("10 transactions", t1[1], t2[1])

# 9. Index scan
print("\n[INDEX]")
t1 = time_mysql(prefix + "SELECT * FROM t WHERE v = 500")
t2 = time_minidb(["SELECT * FROM t WHERE v = 500;"], mdb)
p("INDEX equality", t1[1], t2[1])

t1 = time_mysql(prefix + "SELECT * FROM t WHERE v BETWEEN 400 AND 600")
t2 = time_minidb(["SELECT * FROM t WHERE v BETWEEN 400 AND 600;"], mdb)
p("INDEX range", t1[1], t2[1])

# Cleanup
subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e", "DROP DATABASE bench"],
                shell=False, capture_output=True, text=True, timeout=30)
shutil.rmtree(mdb, ignore_errors=True)

print("\n" + "=" * 75)
print("  Benchmark Complete")
print("=" * 75)
