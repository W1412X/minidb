import threading
#!/usr/bin/env python3
"""
MiniDB Performance Benchmark Suite
测试规模: 100K, 1M, 10M 行
操作: INSERT, SELECT, UPDATE, DELETE, JOIN, AGGREGATE, INDEX

用法:
    python3 perf_benchmark.py ./build/minidb [--scale small|medium|large]
"""
import subprocess, time, sys, tempfile, shutil, os

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"

# Dataset sizes
SCALES = {
    'small':  {'customers': 10000,   'products': 1000,   'orders': 100000,  'ops': 100},
    'medium': {'customers': 100000,  'products': 10000,  'orders': 1000000, 'ops': 500},
    'large':  {'customers': 1000000, 'products': 100000, 'orders': 5000000, 'ops': 1000},
}

scale_name = 'medium'
for arg in sys.argv[2:]:
    if arg.startswith('--scale='):
        scale_name = arg.split('=')[1]
    elif arg in SCALES:
        scale_name = arg

S = SCALES[scale_name]
CUSTOMERS = S['customers']
PRODUCTS = S['products']
ORDERS = S['orders']
OPS = S['ops']

def run_batch(sqls, db_dir):
    """Execute SQL statements in a batch"""
    sqls.append("exit")
    start = time.time()
    subprocess.run([BIN, "--dir", db_dir], input="\n".join(sqls),
                   capture_output=True, text=True, timeout=300)
    return time.time() - start

def run_single(sql, db_dir):
    """Execute a single SQL statement"""
    start = time.time()
    subprocess.run([BIN, "--dir", db_dir], input=sql + "\nexit\n",
                   capture_output=True, text=True, timeout=60)
    return time.time() - start

def run_concurrent(sqls_list, db_dir, n_threads=4):
    """Execute SQL statements concurrently via server"""
    import socket
    port = 15500 + os.getpid() % 500
    server = subprocess.Popen([BIN, "--dir", db_dir, "--server", "--port", str(port)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    def worker(sqls):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(60)
            s.connect(("127.0.0.1", port))
            s.recv(4096)
            for sql in sqls:
                s.sendall((sql + "\n").encode())
            s.sendall("exit\n".encode())
            time.sleep(0.5)
            data = b""
            while True:
                try:
                    chunk = s.recv(65536)
                    if not chunk: break
                    data += chunk
                except: break
            s.close()
            return len(data)
        except Exception as e:
            return 0

    threads = []
    start = time.time()
    for sqls in sqls_list:
        t = threading.Thread(target=worker, args=(sqls,))
        threads.append(t)
        t.start()
    for t in threads:
        t.join(timeout=300)
    elapsed = time.time() - start

    server.terminate()
    server.wait()
    return elapsed

def print_result(op, elapsed, rows=None):
    rate = rows / elapsed if elapsed > 0 and rows else 0
    if rate > 0:
        print(f"  {op:50s}: {elapsed:8.2f}s ({rate:,.0f} ops/s)")
    else:
        print(f"  {op:50s}: {elapsed:8.2f}s")

def print_header():
    print("=" * 70)
    print(f"  MiniDB Performance Benchmark — {scale_name.upper()} Scale")
    print("=" * 70)
    print(f"  Binary:      {BIN}")
    print(f"  Customers:   {CUSTOMERS:,}")
    print(f"  Products:    {PRODUCTS:,}")
    print(f"  Orders:      {ORDERS:,}")
    print(f"  Operations:  {OPS} per test")
    print("=" * 70)

# =================================================================
# Main Benchmark
# =================================================================
print_header()

db = tempfile.mkdtemp(prefix="perf_")

# --- CREATE TABLES ---
print("\n[SETUP]")
t = run_batch(["CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR, email VARCHAR, city VARCHAR);",
               "CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR, price DOUBLE, category VARCHAR);",
               "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, product_id INT, quantity INT, total DOUBLE);"], db)
print(f"  CREATE TABLES: {t:.2f}s")

# Create indexes
t = run_batch(["CREATE INDEX cust_email ON customers (email);",
               "CREATE INDEX cust_city ON customers (city);",
               "CREATE INDEX prod_cat ON products (category);",
               "CREATE INDEX ord_cust ON orders (customer_id);",
               "CREATE INDEX ord_prod ON orders (product_id);"], db)
print(f"  CREATE INDEXES: {t:.2f}s")

# =================================================================
# INSERT Performance
# =================================================================
print("\n[INSERT]")

# Insert customers
sqls = [f"INSERT INTO customers VALUES ({i}, 'Name{i}', 'email{i}@test.com', 'City{i % 50}');" for i in range(1, CUSTOMERS + 1)]
elapsed = run_batch(sqls, db)
print_result(f"INSERT {CUSTOMERS:,} customers", elapsed, CUSTOMERS)

# Insert products
sqls = [f"INSERT INTO products VALUES ({i}, 'Product{i}', {i * 10.0}, 'Cat{i % 10}');" for i in range(1, PRODUCTS + 1)]
elapsed = run_batch(sqls, db)
print_result(f"INSERT {PRODUCTS:,} products", elapsed, PRODUCTS)

# Insert orders (batched)
total_elapsed = 0
batch_size = 1000
for start in range(1, ORDERS + 1, batch_size):
    end = min(start + batch_size - 1, ORDERS)
    sqls = [f"INSERT INTO orders VALUES ({i}, {i % CUSTOMERS + 1}, {i % PRODUCTS + 1}, {i % 10 + 1}, {i * 0.5});" for i in range(start, end + 1)]
    total_elapsed += run_batch(sqls, db)
print_result(f"INSERT {ORDERS:,} orders (batched)", total_elapsed, ORDERS)

# =================================================================
# SELECT Performance
# =================================================================
print("\n[SELECT]")

# COUNT
elapsed = run_single("SELECT COUNT(*) FROM orders;", db)
print_result("COUNT(*) 1M", elapsed)

# SUM
elapsed = run_single("SELECT SUM(total) FROM orders;", db)
print_result("SUM 1M", elapsed)

# AVG
elapsed = run_single("SELECT AVG(total) FROM orders;", db)
print_result("AVG 1M", elapsed)

# Range query
elapsed = run_single("SELECT COUNT(*) FROM orders WHERE total > 500000;", db)
print_result("COUNT WHERE total>500K", elapsed)

# Point queries (100)
sqls = [f"SELECT name FROM customers WHERE id = {i};" for i in range(1, 101)]
elapsed = run_batch(sqls, db)
print_result(f"100 point queries", elapsed)

# Multi-range
elapsed = run_single("SELECT * FROM orders WHERE total BETWEEN 100000 AND 500000;", db)
print_result("BETWEEN range 100K-500K", elapsed)

# =================================================================
# INDEX Performance
# =================================================================
print("\n[INDEX]")

# Equality lookup via index
sqls = [f"SELECT id FROM customers WHERE email = 'email{i}@test.com';" for i in range(1, 101)]
elapsed = run_batch(sqls, db)
print_result("100 index equality lookups", elapsed)

# Range via index
elapsed = run_single("SELECT COUNT(*) FROM orders WHERE customer_id BETWEEN 1 AND 1000;", db)
print_result("Index range (customer_id)", elapsed)

# =================================================================
# AGGREGATE Performance
# =================================================================
print("\n[AGGREGATE]")

# GROUP BY
elapsed = run_single("SELECT city, COUNT(*) FROM customers GROUP BY city ORDER BY COUNT(*) DESC LIMIT 5;", db)
print_result("GROUP BY + ORDER BY", elapsed)

# Complex aggregate
elapsed = run_single("SELECT customer_id, COUNT(*) AS cnt, SUM(total) AS tot FROM orders GROUP BY customer_id ORDER BY tot DESC LIMIT 10;", db)
print_result("GROUP BY + ORDER BY on 1M", elapsed)

# HAVING
elapsed = run_single("SELECT customer_id, COUNT(*) FROM orders GROUP BY customer_id HAVING COUNT(*) > 100;", db)
print_result("GROUP BY + HAVING 1M", elapsed)

# =================================================================
# JOIN Performance
# =================================================================
print("\n[JOIN]")

# INNER JOIN
elapsed = run_single("SELECT COUNT(*) FROM customers c INNER JOIN orders o ON c.id = o.customer_id;", db)
print_result("INNER JOIN 1M", elapsed)

# LEFT JOIN
elapsed = run_single("SELECT COUNT(*) FROM customers c LEFT JOIN orders o ON c.id = o.customer_id;", db)
print_result("LEFT JOIN 1M", elapsed)

# Aggregated JOIN
elapsed = run_single("SELECT c.name, COUNT(o.id), SUM(o.total) FROM customers c LEFT JOIN orders o ON c.id = o.customer_id GROUP BY c.name ORDER BY SUM(o.total) DESC LIMIT 5;", db)
print_result("LEFT JOIN + GROUP BY + ORDER BY", elapsed)

# =================================================================
# UPDATE Performance
# =================================================================
print("\n[UPDATE]")

# Single row
elapsed = run_single("UPDATE customers SET city = 'Updated' WHERE id = 1;", db)
print_result("UPDATE 1 row", elapsed)

# Batch update
sqls = [f"UPDATE customers SET city = 'Updated{i}' WHERE id = {i};" for i in range(1, 101)]
elapsed = run_batch(sqls, db)
print_result("UPDATE 100 rows", elapsed)

# Bulk update
elapsed = run_single("UPDATE orders SET total = total * 1.1 WHERE customer_id = 1;", db)
print_result("UPDATE by index (customer_id=1)", elapsed)

# Bulk all
elapsed = run_single("UPDATE customers SET city = 'NewCity';", db)
print_result("UPDATE ALL customers", elapsed)

# =================================================================
# DELETE Performance
# =================================================================
print("\n[DELETE]")

# Single row
elapsed = run_single("DELETE FROM customers WHERE id = 1;", db)
print_result("DELETE 1 row", elapsed)

# Batch delete
sqls = [f"DELETE FROM customers WHERE id = {i};" for i in range(2, 102)]
elapsed = run_batch(sqls, db)
print_result("DELETE 100 rows", elapsed)

# Bulk delete by range
elapsed = run_single("DELETE FROM orders WHERE total < 1000;", db)
print_result("DELETE by range (<1000)", elapsed)

# =================================================================
# TRANSACTION Performance
# =================================================================
print("\n[TRANSACTION]")

# Single transaction
sqls = ["BEGIN;", "INSERT INTO customers VALUES (99999, 'test', 'test@test.com', 'test');", "COMMIT;"]
elapsed = run_batch(sqls, db)
print_result("1 transaction (BEGIN+INSERT+COMMIT)", elapsed)

# Multiple transactions
sqls = []
for i in range(1, 51):
    sqls.append("BEGIN;")
    sqls.append(f"INSERT INTO customers VALUES ({CUSTOMERS + i}, 't{i}', 't{i}@test.com', 'c{i % 50}');")
    sqls.append("COMMIT;")
elapsed = run_batch(sqls, db)
print_result("50 transactions", elapsed, 50)

# ROLLBACK
sqls = ["BEGIN;", "INSERT INTO customers VALUES (99998, 'test2', 'test2@test.com', 'test2');", "ROLLBACK;"]
elapsed = run_batch(sqls, db)
print_result("ROLLBACK", elapsed)

# =================================================================
# CONCURRENT Performance
# =================================================================
print("\n[CONCURRENT] (server mode)")

# Concurrent inserts
sqls_list = [[f"INSERT INTO orders VALUES ({ORDERS + tid*1000 + i}, {tid}, {i % 100}, {i % 10 + 1}, {i * 0.5});" for i in range(1, 101)] for tid in range(1, 5)]
elapsed = run_concurrent(sqls_list, db, n_threads=4)
print_result("4 threads × 100 INSERTs", elapsed, 400)

# Concurrent selects
sqls_list = [[f"SELECT v FROM orders WHERE id = {i};" for i in range(1, 101)] for _ in range(4)]
elapsed = run_concurrent(sqls_list, db, n_threads=4)
print_result("4 threads × 100 SELECTs", elapsed, 400)

# =================================================================
# Cleanup
# =================================================================
shutil.rmtree(db, ignore_errors=True)

print("\n" + "=" * 70)
print(f"  Benchmark Complete ({scale_name} scale)")
print("=" * 70)
