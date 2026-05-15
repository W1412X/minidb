#!/usr/bin/env python3
"""
MySQL vs MiniDB Large-Scale Benchmark
Dataset: 1,000,000 rows customers, 10,000 products, 5,000,000 orders
"""
import subprocess, time, statistics, tempfile, shutil, sys, os

MINIDB = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
ITERATIONS = 2  # 3x per test for large datasets
CUSTOMERS = 1000000
PRODUCTS = 10000
ORDERS = 5000000

def time_mysql(sql, iterations=ITERATIONS):
    times = []
    for _ in range(iterations):
        start = time.time()
        subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e", sql],
                       capture_output=True, text=True, timeout=600)
        elapsed = time.time() - start
        if elapsed > 0: times.append(elapsed)
    return min(times) if times else 0, statistics.mean(times) if times else 0, max(times) if times else 0

def time_minidb(sqls, db_dir, iterations=ITERATIONS):
    times = []
    for _ in range(iterations):
        sqls.append("exit")
        start = time.time()
        subprocess.run([MINIDB, "--dir", db_dir], input="\n".join(sqls),
                       capture_output=True, text=True, timeout=600)
        elapsed = time.time() - start
        if elapsed > 0: times.append(elapsed)
        sqls.pop()
    return min(times) if times else 0, statistics.mean(times) if times else 0, max(times) if times else 0

def p(op, mt, md):
    if md > 0:
        ratio = mt/md
        w = "MiniDB" if md < mt else "MySQL"
        print(f"  {op:40s}: MySQL {mt:8.2f}s | MiniDB {md:8.2f}s | {w} {ratio:.1f}x")
    else:
        print(f"  {op:40s}: MySQL {mt:8.2f}s | MiniDB {md:8.2f}s | ERROR")

def generate_sql_batch(n, table, gen_fn):
    """Generate INSERT statements in batches"""
    batch = []
    for i in range(1, n + 1):
        batch.append(gen_fn(i))
        if len(batch) >= 1000:
            yield "; ".join(batch)
            batch = []
    if batch:
        yield "; ".join(batch)

print("=" * 80)
print("  MySQL vs MiniDB — Large-Scale Benchmark")
print("=" * 80)
print(f"  Dataset: {CUSTOMERS:,} customers, {PRODUCTS:,} products, {ORDERS:,} orders")
print(f"  MySQL:   8.0.46 (localhost, root)")
print(f"  MiniDB:  {MINIDB}")
print(f"  Itr:     {ITERATIONS}x per test")
print("=" * 80)

# === MySQL Setup ===
print("\n[Setup] MySQL...")
subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e",
                 "DROP DATABASE IF EXISTS bench; CREATE DATABASE bench"],
                shell=False, capture_output=True, text=True, timeout=60)
subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e",
                 "USE bench; CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(100), email VARCHAR(100), city VARCHAR(50))"],
                shell=False, capture_output=True, text=True, timeout=60)
subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e",
                 "USE bench; CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR(100), price DOUBLE, category VARCHAR(50))"],
                shell=False, capture_output=True, text=True, timeout=60)
subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e",
                 "USE bench; CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, product_id INT, quantity INT, total DOUBLE)"],
                shell=False, capture_output=True, text=True, timeout=60)

# === MiniDB Setup ===
print("[Setup] MiniDB...")
mdb = tempfile.mkdtemp(prefix="bench_")
subprocess.run([MINIDB, "--dir", mdb], input="\n".join([
    "CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR, email VARCHAR, city VARCHAR);",
    "CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR, price DOUBLE, category VARCHAR);",
    "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, product_id INT, quantity INT, total DOUBLE);",
    "CREATE INDEX cust_city ON customers (city);",
    "CREATE INDEX prod_cat ON products (category);",
    "CREATE INDEX ord_cust ON orders (customer_id);",
    "CREATE INDEX ord_prod ON orders (product_id);",
    "exit"]),
    capture_output=True, text=True, timeout=30)

# === INSERT Tests ===
print("\n--- INSERT Performance ---")

# Insert 1M customers
print("  Inserting 1M customers (MySQL)...")
mysql_ins = time_mysql("; ".join([f"INSERT INTO customers VALUES ({i}, 'Name{i}', 'email{i}@test.com', 'City{i % 50}')" for i in range(1, 1001)]))
# For MySQL, we need to use smaller batches
mysql_sqls = []
for batch_start in range(1, CUSTOMERS + 1, 1000):
    batch_end = min(batch_start + 999, CUSTOMERS)
    batch = "; ".join([f"INSERT INTO customers VALUES ({i}, 'Name{i}', 'email{i}@test.com', 'City{i % 50}')" for i in range(batch_start, batch_end + 1)])
    mysql_sqls.append(f"USE bench; {batch}")
start = time.time()
for batch_sql in mysql_sqls:
    subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e", batch_sql],
                   shell=False, capture_output=True, text=True, timeout=300)
mysql_total = time.time() - start
print(f"  MySQL INSERT {CUSTOMERS:,} customers: {mysql_total:.1f}s")

print("  Inserting 1M customers (MiniDB)...")
mdb_ins_sqls = [f"INSERT INTO customers VALUES ({i}, 'Name{i}', 'email{i}@test.com', 'City{i % 50}');" for i in range(1, CUSTOMERS + 1)]
start = time.time()
subprocess.run([MINIDB, "--dir", mdb], input="\n".join(mdb_ins_sqls + ["exit"]),
               capture_output=True, text=True, timeout=600)
mdb_total = time.time() - start
print(f"  MiniDB INSERT {CUSTOMERS:,} customers: {mdb_total:.1f}s")
p("INSERT 1M customers", mysql_total, mdb_total)

# Insert 10K products
print("  Inserting 10K products...")
mysql_sqls = "; ".join([f"INSERT INTO products VALUES ({i}, 'Product{i}', {i*10.0}, 'Cat{i % 10}')" for i in range(1, PRODUCTS + 1)])
start = time.time()
subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e", f"USE bench; {mysql_sqls}"],
               shell=False, capture_output=True, text=True, timeout=300)
mysql_total = time.time() - start

mdb_sqls = [f"INSERT INTO products VALUES ({i}, 'Product{i}', {i*10.0}, 'Cat{i % 10}');" for i in range(1, PRODUCTS + 1)]
start = time.time()
subprocess.run([MINIDB, "--dir", mdb], input="\n".join(mdb_sqls + ["exit"]),
               capture_output=True, text=True, timeout=300)
mdb_total = time.time() - start
p("INSERT 10K products", mysql_total, mdb_total)

# Insert 5M orders (MySQL batch, MiniDB batch)
print("  Inserting 5M orders (MySQL batch)...")
start = time.time()
for batch_start in range(1, ORDERS + 1, 5000):
    batch_end = min(batch_start + 4999, ORDERS)
    batch = "; ".join([f"INSERT INTO orders VALUES ({i}, {i % CUSTOMERS + 1}, {i % PRODUCTS + 1}, {i % 10 + 1}, {i * 0.5})" for i in range(batch_start, batch_end + 1)])
    subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e", f"USE bench; {batch}"],
                   shell=False, capture_output=True, text=True, timeout=600)
mysql_total = time.time() - start
print(f"  MySQL INSERT {ORDERS:,} orders: {mysql_total:.1f}s")

print("  Inserting 5M orders (MiniDB batch)...")
mdb_batches = [f"INSERT INTO orders VALUES ({i}, {i % CUSTOMERS + 1}, {i % PRODUCTS + 1}, {i % 10 + 1}, {i * 0.5});" for i in range(1, ORDERS + 1)]
start = time.time()
subprocess.run([MINIDB, "--dir", mdb], input="\n".join(mdb_batches + ["exit"]),
               capture_output=True, text=True, timeout=900)
mdb_total = time.time() - start
print(f"  MiniDB INSERT {ORDERS:,} orders: {mdb_total:.1f}s")
p("INSERT 5M orders", mysql_total, mdb_total)

# === SELECT Tests ===
print("\n--- SELECT Performance ---")

# Count queries
mysql_sel = time_mysql("USE bench; SELECT COUNT(*) FROM orders")
mdb_sel = time_minidb(["SELECT COUNT(*) FROM orders;"], mdb)
p("COUNT(*) 5M rows", mysql_sel[1], mdb_sel[1])

mysql_sel = time_mysql("USE bench; SELECT COUNT(*) FROM customers")
mdb_sel = time_minidb(["SELECT COUNT(*) FROM customers;"], mdb)
p("COUNT(*) 1M customers", mysql_sel[1], mdb_sel[1])

# Aggregates
mysql_agg = time_mysql("USE bench; SELECT COUNT(*), SUM(total), AVG(total), MIN(total), MAX(total) FROM orders")
mdb_agg = time_minidb(["SELECT COUNT(*), SUM(total), AVG(total), MIN(total), MAX(total) FROM orders;"], mdb)
p("5 Aggregates 5M rows", mysql_agg[1], mdb_agg[1])

# Point queries (100)
mysql_sel = time_mysql("USE bench; " + "; ".join([f"SELECT name FROM customers WHERE id = {i}" for i in range(1, 101)]))
mdb_sel = time_minidb([f"SELECT name FROM customers WHERE id = {i};" for i in range(1, 101)], mdb)
p("100 point queries", mysql_sel[1], mdb_sel[1])

# Range queries
mysql_sel = time_mysql("USE bench; SELECT COUNT(*) FROM orders WHERE total > 100000")
mdb_sel = time_minidb(["SELECT COUNT(*) FROM orders WHERE total > 100000;"], mdb)
p("Range query (total>100K)", mysql_sel[1], mdb_sel[1])

# GROUP BY
mysql_agg = time_mysql("USE bench; SELECT city, COUNT(*) FROM customers GROUP BY city ORDER BY COUNT(*) DESC LIMIT 5")
mdb_agg = time_minidb(["SELECT city, COUNT(*) FROM customers GROUP BY city ORDER BY COUNT(*) DESC LIMIT 5;"], mdb)
p("GROUP BY + ORDER BY + LIMIT", mysql_agg[1], mdb_agg[1])

# JOIN
mysql_join = time_mysql("USE bench; SELECT c.name, COUNT(o.id) FROM customers c LEFT JOIN orders o ON c.id = o.customer_id GROUP BY c.name ORDER BY COUNT(o.id) DESC LIMIT 5")
mdb_join = time_minidb(["SELECT c.name, COUNT(o.id) FROM customers c LEFT JOIN orders o ON c.id = o.customer_id GROUP BY c.name ORDER BY COUNT(o.id) DESC LIMIT 5;"], mdb)
p("LEFT JOIN + GROUP BY + ORDER BY", mysql_join[1], mdb_join[1])

# IN subquery
mysql_sub = time_mysql("USE bench; SELECT * FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE total > 500000) LIMIT 10")
mdb_sub = time_minidb(["SELECT * FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE total > 500000) LIMIT 10;"], mdb)
p("IN subquery (5M rows)", mysql_sub[1], mdb_sub[1])

# === UPDATE Tests ===
print("\n--- UPDATE Performance ---")

mysql_upd = time_mysql("USE bench; " + "; ".join([f"UPDATE customers SET city = 'Updated' WHERE id = {i}" for i in range(1, 101)]))
mdb_upd = time_minidb([f"UPDATE customers SET city = 'Updated' WHERE id = {i};" for i in range(1, 101)], mdb)
p("UPDATE 100 customers", mysql_upd[1], mdb_upd[1])

mysql_upd = time_mysql("USE bench; UPDATE customers SET city = 'NewCity'")
mdb_upd = time_minidb(["UPDATE customers SET city = 'NewCity';"], mdb)
p("UPDATE all 1M customers", mysql_upd[1], mdb_upd[1])

mysql_upd = time_mysql("USE bench; UPDATE orders SET total = total * 1.1 WHERE customer_id = 1")
mdb_upd = time_minidb(["UPDATE orders SET total = total * 1.1 WHERE customer_id = 1;"], mdb)
p("UPDATE by indexed col", mysql_upd[1], mdb_upd[1])

# === DELETE Tests ===
print("\n--- DELETE Performance ---")

mysql_del = time_mysql("USE bench; " + "; ".join([f"DELETE FROM customers WHERE id = {i}" for i in range(1, 101)]))
mdb_del = time_minidb([f"DELETE FROM customers WHERE id = {i};" for i in range(1, 101)], mdb)
p("DELETE 100 customers", mysql_del[1], mdb_del[1])

mysql_del = time_mysql("USE bench; DELETE FROM orders WHERE total < 1000")
mdb_del = time_minidb(["DELETE FROM orders WHERE total < 1000;"], mdb)
p("DELETE by range (5M rows)", mysql_del[1], mdb_del[1])

# === Cleanup ===
print("\n[Cleanup]")
subprocess.run(["mysql", "-u", "root", "--default-auth=mysql_native_password", "-e", "DROP DATABASE bench"],
                shell=False, capture_output=True, text=True, timeout=60)
shutil.rmtree(mdb, ignore_errors=True)

print("\n" + "=" * 80)
print("  Large-Scale Benchmark Complete")
print("=" * 80)
