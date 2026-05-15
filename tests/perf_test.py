#!/usr/bin/env python3
"""
MiniDB 性能测试 — 系统性性能分析
"""
import subprocess, time, os, tempfile, shutil, sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"

class DB:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="minidb_perf_")
    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)
    def run(self, sqls):
        if isinstance(sqls, str): sqls = [sqls]
        sqls.append("exit")
        r = subprocess.run([BIN, "--dir", self.dir], input="\n".join(sqls),
                           capture_output=True, text=True, timeout=300)
        return r.stdout + r.stderr

def run_sql(db_dir, sql):
    """Run a single SQL statement and return output"""
    r = subprocess.run([BIN, "--dir", db_dir], input=sql + "\nexit\n",
                       capture_output=True, text=True, timeout=300)
    return r.stdout

def benchmark(desc, sql_list, db_dir=None, iterations=1):
    """Benchmark a sequence of SQL statements"""
    if db_dir is None:
        db_dir = tempfile.mkdtemp(prefix="perf_")
        cleanup = True
    else:
        cleanup = False
    
    total_time = 0
    for _ in range(iterations):
        start = time.time()
        sqls = [s for s in sql_list] + ["exit"]
        subprocess.run([BIN, "--dir", db_dir], input="\n".join(sqls),
                      capture_output=True, text=True, timeout=300)
        total_time += time.time() - start
    
    avg_time = total_time / iterations
    
    # Count operations
    ops = sum(1 for s in sql_list if s.strip().upper().startswith(("INSERT", "UPDATE", "DELETE", "SELECT")))
    
    print(f"  {desc}: {avg_time:.3f}s ({ops/avg_time:.0f} ops/s)" if ops else f"  {desc}: {avg_time:.3f}s")
    
    if cleanup:
        shutil.rmtree(db_dir, ignore_errors=True)
    
    return avg_time, ops

def batch_insert(n, db_dir):
    """Benchmark N INSERTs"""
    sqls = [f"INSERT INTO t VALUES ({i}, {i})" for i in range(1, n+1)]
    return benchmark(f"INSERT {n} rows", sqls, db_dir)

def batch_update(n, db_dir):
    """Benchmark N UPDATEs"""
    sqls = [f"UPDATE t SET v = v + 1 WHERE id = {i}" for i in range(1, n+1)]
    return benchmark(f"UPDATE {n} rows", sqls, db_dir)

def batch_delete(n, db_dir):
    """Benchmark N DELETEs"""
    sqls = [f"DELETE FROM t WHERE id = {i}" for i in range(1, n+1)]
    return benchmark(f"DELETE {n} rows", sqls, db_dir)

def batch_select(n, db_dir):
    """Benchmark N SELECTs"""
    sqls = [f"SELECT v FROM t WHERE id = {i}" for i in range(1, n+1)]
    return benchmark(f"SELECT {n} rows", sqls, db_dir)

def run_all_benchmarks():
    db = DB()
    print("=" * 60)
    print("MiniDB Performance Benchmark")
    print("=" * 60)
    
    # Setup
    print("\n--- INSERT Performance ---")
    db.run(["CREATE TABLE t (id INT PRIMARY KEY, v INT);"])
    
    # Test 1: Single INSERT
    benchmark("1 INSERT", ["INSERT INTO t VALUES (1, 10);"], db.dir)
    
    # Test 2: Batch INSERT (10)
    db.run(["DELETE FROM t;"])
    benchmark("10 INSERTs", [f"INSERT INTO t VALUES ({i}, {i});" for i in range(1, 11)], db.dir)
    
    # Test 3: Batch INSERT (100)
    db.run(["DELETE FROM t;"])
    benchmark("100 INSERTs", [f"INSERT INTO t VALUES ({i}, {i});" for i in range(1, 101)], db.dir)
    
    # Test 4: Batch INSERT (1000)
    db.run(["DELETE FROM t;"])
    benchmark("1000 INSERTs", [f"INSERT INTO t VALUES ({i}, {i});" for i in range(1, 1001)], db.dir)
    
    # Test 5: Multi-value INSERT
    db.run(["DELETE FROM t;"])
    sqls = []
    for i in range(1, 101):
        sqls.append(f"INSERT INTO t VALUES ({i}, {i}), ({i+100}, {i+100}), ({i+200}, {i+200});")
    benchmark("100 Multi-value INSERTs (300 rows)", sqls, db.dir)
    
    # Reset
    db.run(["DELETE FROM t;"])
    for i in range(1, 1001):
        db.run([f"INSERT INTO t VALUES ({i}, {i});"])
    
    print("\n--- UPDATE Performance ---")
    benchmark("100 UPDATEs", [f"UPDATE t SET v = v + 1 WHERE id = {i};" for i in range(1, 101)], db.dir)
    benchmark("1000 UPDATEs", [f"UPDATE t SET v = v + 1 WHERE id = {i};" for i in range(1, 1001)], db.dir)
    benchmark("1 UPDATE all rows", ["UPDATE t SET v = v + 1;"], db.dir)
    
    print("\n--- SELECT Performance ---")
    benchmark("100 SELECTs", [f"SELECT v FROM t WHERE id = {i};" for i in range(1, 101)], db.dir)
    benchmark("1000 SELECTs", [f"SELECT v FROM t WHERE id = {i};" for i in range(1, 1001)], db.dir)
    benchmark("1 SELECT COUNT", ["SELECT COUNT(*) FROM t;"], db.dir)
    benchmark("1 SELECT SUM", ["SELECT SUM(v) FROM t;"], db.dir)
    benchmark("1 SELECT ALL", ["SELECT * FROM t;"], db.dir)
    
    print("\n--- DELETE Performance ---")
    db.run(["DELETE FROM t;"])
    for i in range(1, 1001):
        db.run([f"INSERT INTO t VALUES ({i}, {i});"])
    benchmark("100 DELETEs", [f"DELETE FROM t WHERE id = {i};" for i in range(1, 101)], db.dir)
    
    print("\n--- JOIN Performance ---")
    db.run(["CREATE TABLE j1 (id INT PRIMARY KEY, v INT);"])
    db.run(["CREATE TABLE j2 (id INT PRIMARY KEY, v INT);"])
    for i in range(1, 101):
        db.run([f"INSERT INTO j1 VALUES ({i}, {i});", f"INSERT INTO j2 VALUES ({i}, {i*10});"])
    benchmark("INNER JOIN (100x100)", ["SELECT COUNT(*) FROM j1 INNER JOIN j2 ON j1.id = j2.id;"], db.dir)
    benchmark("LEFT JOIN (100x100)", ["SELECT COUNT(*) FROM j1 LEFT JOIN j2 ON j1.id = j2.id;"], db.dir)
    
    print("\n--- Transaction Performance ---")
    db.run(["CREATE TABLE txn_t (id INT PRIMARY KEY, v INT);"])
    db.run(["BEGIN; INSERT INTO txn_t VALUES (1, 10); COMMIT;"])
    benchmark("100 BEGIN/INSERT/COMMIT",
              ["BEGIN;", "INSERT INTO txn_t VALUES (1, 10);", "COMMIT;"] * 100,
              db.dir)
    
    print("\n--- Expression Performance ---")
    benchmark("100 arithmetic ops", [f"SELECT {i} + {i+1};" for i in range(1, 101)], db.dir)
    benchmark("100 CASE expressions",
              [f"SELECT CASE WHEN {i} > 5 THEN 'big' ELSE 'small' END;" for i in range(1, 101)],
              db.dir)
    
    db.cleanup()
    print("\n" + "=" * 60)
    print("Benchmark Complete")
    print("=" * 60)

if __name__ == "__main__":
    run_all_benchmarks()
