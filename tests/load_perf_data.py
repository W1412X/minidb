#!/usr/bin/env python3
"""
Load deterministic performance-test data into MiniADB.

Default dataset:
  - 8 tables
  - orders:      1,000,001 rows
  - order_items: 1,200,000 rows
  - joinable via customer_id, order_id, product_id, store_id, date_id

Use --preset smoke for a fast correctness check before the full load.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


PRESETS = {
    "smoke": {
        "customers": 100,
        "products": 50,
        "stores": 10,
        "dates": 30,
        "orders": 1_000,
        "items": 1_500,
        "payments": 1_000,
        "shipments": 900,
        "batch": 40,
        "commit_every": 100,
        "progress_every": 1_000,
    },
    "full": {
        "customers": 100_000,
        "products": 10_000,
        "stores": 1_000,
        "dates": 3_650,
        "orders": 1_000_001,
        "items": 1_200_000,
        "payments": 1_000_001,
        "shipments": 950_000,
        "batch": 40,
        "commit_every": 1_000,
        "progress_every": 10_000,
    },
}


def sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def run_statement(proc: subprocess.Popen[str], sql: str) -> None:
    assert proc.stdin is not None
    proc.stdin.write(sql)
    if not sql.endswith("\n"):
        proc.stdin.write("\n")


def batched_insert(
    proc: subprocess.Popen[str],
    table: str,
    rows,
    batch_size: int,
    commit_every: int,
    progress_every: int,
) -> int:
    batch: list[str] = []
    count = 0
    statements = 0
    next_progress = progress_every
    t0 = time.time()

    run_statement(proc, "BEGIN;")
    for row in rows:
        batch.append("(" + ", ".join(row) + ")")
        count += 1
        if len(batch) >= batch_size:
            run_statement(proc, f"INSERT INTO {table} VALUES " + ", ".join(batch) + ";")
            statements += 1
            batch.clear()
            if commit_every > 0 and statements % commit_every == 0:
                run_statement(proc, "COMMIT;")
                run_statement(proc, "BEGIN;")
            if progress_every > 0 and count >= next_progress:
                elapsed = time.time() - t0
                rate = count / elapsed if elapsed > 0 else 0
                print(
                    f"  {table:12s} progress {count:>10,d} rows ({rate:,.0f} rows/s)",
                    file=sys.stderr,
                )
                next_progress += progress_every
    if batch:
        run_statement(proc, f"INSERT INTO {table} VALUES " + ", ".join(batch) + ";")
        statements += 1
    run_statement(proc, "COMMIT;")
    return count


def dimension_customers(n: int):
    segments = ("retail", "smb", "enterprise", "public")
    regions = ("north", "south", "east", "west", "central")
    for i in range(1, n + 1):
        yield (
            str(i),
            sql_string(f"customer_{i}"),
            sql_string(segments[i % len(segments)]),
            sql_string(regions[i % len(regions)]),
        )


def dimension_products(n: int):
    categories = ("book", "electronics", "home", "sports", "office", "grocery")
    for i in range(1, n + 1):
        price = 5 + (i % 500) * 1.25
        yield (
            str(i),
            sql_string(f"product_{i}"),
            sql_string(categories[i % len(categories)]),
            f"{price:.2f}",
        )


def dimension_stores(n: int):
    regions = ("north", "south", "east", "west", "central")
    for i in range(1, n + 1):
        yield (str(i), sql_string(f"store_{i}"), sql_string(regions[i % len(regions)]))


def dimension_dates(n: int):
    for i in range(1, n + 1):
        year = 2020 + ((i - 1) // 365)
        month = ((i - 1) // 30) % 12 + 1
        day = ((i - 1) % 28) + 1
        yield (str(i), str(year), str(month), str(day))


def fact_orders(n: int, customers: int, stores: int, dates: int):
    statuses = ("new", "paid", "shipped", "closed", "returned")
    for i in range(1, n + 1):
        customer_id = (i * 17) % customers + 1
        store_id = (i * 7) % stores + 1
        date_id = (i * 13) % dates + 1
        total = 20 + (i % 10_000) * 0.37
        yield (
            str(i),
            str(customer_id),
            str(store_id),
            str(date_id),
            f"{total:.2f}",
            sql_string(statuses[i % len(statuses)]),
        )


def fact_order_items(n: int, orders: int, products: int):
    for i in range(1, n + 1):
        order_id = (i * 19) % orders + 1
        product_id = (i * 23) % products + 1
        quantity = i % 5 + 1
        unit_price = 5 + (product_id % 500) * 1.25
        yield (
            str(i),
            str(order_id),
            str(product_id),
            str(quantity),
            f"{unit_price:.2f}",
        )


def fact_payments(n: int, orders: int):
    methods = ("card", "cash", "wire", "wallet")
    for i in range(1, n + 1):
        order_id = i if i <= orders else (i % orders) + 1
        amount = 20 + (order_id % 10_000) * 0.37
        yield (
            str(i),
            str(order_id),
            f"{amount:.2f}",
            sql_string(methods[i % len(methods)]),
        )


def fact_shipments(n: int, orders: int, stores: int):
    carriers = ("ups", "fedex", "dhl", "postal")
    for i in range(1, n + 1):
        order_id = (i * 29) % orders + 1
        warehouse_id = (i * 11) % stores + 1
        yield (
            str(i),
            str(order_id),
            str(warehouse_id),
            sql_string(carriers[i % len(carriers)]),
        )


def exec_sql(proc: subprocess.Popen[str], config: dict[str, int]) -> None:
    schema = [
        "CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR, segment VARCHAR, region VARCHAR);",
        "CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR, category VARCHAR, price FLOAT);",
        "CREATE TABLE stores (id INT PRIMARY KEY, name VARCHAR, region VARCHAR);",
        "CREATE TABLE dates (id INT PRIMARY KEY, year INT, month INT, day INT);",
        "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, store_id INT, date_id INT, total FLOAT, status VARCHAR);",
        "CREATE TABLE order_items (id INT PRIMARY KEY, order_id INT, product_id INT, quantity INT, unit_price FLOAT);",
        "CREATE TABLE payments (id INT PRIMARY KEY, order_id INT, amount FLOAT, method VARCHAR);",
        "CREATE TABLE shipments (id INT PRIMARY KEY, order_id INT, warehouse_id INT, carrier VARCHAR);",
    ]
    for statement in schema:
        run_statement(proc, statement)

    batch = config["batch"]
    jobs = [
        ("customers", dimension_customers(config["customers"])),
        ("products", dimension_products(config["products"])),
        ("stores", dimension_stores(config["stores"])),
        ("dates", dimension_dates(config["dates"])),
        ("orders", fact_orders(config["orders"], config["customers"], config["stores"], config["dates"])),
        ("order_items", fact_order_items(config["items"], config["orders"], config["products"])),
        ("payments", fact_payments(config["payments"], config["orders"])),
        ("shipments", fact_shipments(config["shipments"], config["orders"], config["stores"])),
    ]

    start = time.time()
    for table, rows in jobs:
        t0 = time.time()
        count = batched_insert(
            proc,
            table,
            rows,
            batch,
            config["commit_every"],
            config["progress_every"],
        )
        proc.stdin.flush()  # type: ignore[union-attr]
        elapsed = time.time() - t0
        print(f"loaded {table:12s} {count:>10,d} rows in {elapsed:8.2f}s", file=sys.stderr)

    indexes = [
        "CREATE INDEX idx_orders_customer ON orders (customer_id);",
        "CREATE INDEX idx_orders_store ON orders (store_id);",
        "CREATE INDEX idx_orders_date ON orders (date_id);",
        "CREATE INDEX idx_items_order ON order_items (order_id);",
        "CREATE INDEX idx_items_product ON order_items (product_id);",
        "CREATE INDEX idx_payments_order ON payments (order_id);",
        "CREATE INDEX idx_shipments_order ON shipments (order_id);",
    ]
    for statement in indexes:
        run_statement(proc, statement)

    checks = [
        "SELECT COUNT(*) AS customers FROM customers;",
        "SELECT COUNT(*) AS orders FROM orders;",
        "SELECT COUNT(*) AS order_items FROM order_items;",
        "EXPLAIN SELECT * FROM orders WHERE id = 42;",
        "EXPLAIN SELECT * FROM order_items WHERE order_id = 42;",
    ]
    for statement in checks:
        run_statement(proc, statement)

    run_statement(proc, "exit")
    print(f"total load stream time: {time.time() - start:.2f}s", file=sys.stderr)


def write_query_file(db_dir: Path) -> None:
    queries = """\
-- MiniADB performance probe queries.
-- Run with: build/minidb --dir perf_data < perf_data/perf_queries.sql

EXPLAIN SELECT * FROM orders WHERE id = 424242;
SELECT * FROM orders WHERE id = 424242;

EXPLAIN SELECT * FROM order_items WHERE order_id = 424242;
SELECT COUNT(*) AS items_for_order FROM order_items WHERE order_id = 424242;

SELECT COUNT(*) AS orders FROM orders;
SELECT COUNT(*) AS order_items FROM order_items;

SELECT c.id, c.region, o.id, o.total FROM customers c INNER JOIN orders o ON c.id = o.customer_id WHERE o.id = 424242;

SELECT o.id, i.product_id, i.quantity, i.unit_price FROM orders o INNER JOIN order_items i ON o.id = i.order_id WHERE o.id = 424242 LIMIT 20;

SELECT p.category, COUNT(*) AS line_count FROM products p INNER JOIN order_items i ON p.id = i.product_id GROUP BY p.category ORDER BY line_count DESC;

exit
"""
    (db_dir / "perf_queries.sql").write_text(queries)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Load MiniADB performance dataset")
    parser.add_argument("--bin", default=str(ROOT / "build" / "minidb"), help="path to minidb binary")
    parser.add_argument("--dir", default=str(ROOT / "perf_data"), help="database directory")
    parser.add_argument("--preset", choices=sorted(PRESETS), default="full", help="dataset size")
    parser.add_argument("--clean", action="store_true", help="delete database directory before loading")
    parser.add_argument("--customers", type=int, help="override customers row count")
    parser.add_argument("--products", type=int, help="override products row count")
    parser.add_argument("--stores", type=int, help="override stores row count")
    parser.add_argument("--dates", type=int, help="override dates row count")
    parser.add_argument("--orders", type=int, help="override orders row count")
    parser.add_argument("--items", type=int, help="override order_items row count")
    parser.add_argument("--payments", type=int, help="override payments row count")
    parser.add_argument("--shipments", type=int, help="override shipments row count")
    parser.add_argument("--batch", type=int, help="rows per INSERT statement")
    parser.add_argument("--commit-every", type=int, help="commit every N INSERT statements")
    parser.add_argument("--progress-every", type=int, help="print progress every N rows")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    binary = Path(args.bin)
    db_dir = Path(args.dir)
    if not binary.exists():
        print(f"minidb binary not found: {binary}", file=sys.stderr)
        print("run: cmake --build build", file=sys.stderr)
        return 2

    if args.clean and db_dir.exists():
        shutil.rmtree(db_dir)
    db_dir.mkdir(parents=True, exist_ok=True)
    write_query_file(db_dir)

    config = dict(PRESETS[args.preset])
    for key in ("customers", "products", "stores", "dates", "orders", "items", "payments", "shipments"):
        value = getattr(args, key)
        if value is not None:
            config[key] = value
    if args.batch is not None:
        config["batch"] = args.batch
    if args.commit_every is not None:
        config["commit_every"] = args.commit_every
    if args.progress_every is not None:
        config["progress_every"] = args.progress_every

    print(f"loading preset={args.preset} into {db_dir}", file=sys.stderr)
    print(
        f"orders={config['orders']:,} order_items={config['items']:,} "
        f"batch={config['batch']} commit_every={config['commit_every']}",
        file=sys.stderr,
    )

    env = os.environ.copy()
    log_path = db_dir / "load.log"
    with log_path.open("w") as log_file:
        proc = subprocess.Popen(
            [str(binary), "--dir", str(db_dir)],
            stdin=subprocess.PIPE,
            stdout=log_file,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env,
        )
        assert proc.stdin is not None
        try:
            exec_sql(proc, config)
            proc.stdin.close()
            stderr = proc.stderr.read() if proc.stderr else ""
            rc = proc.wait()
        except BrokenPipeError:
            stderr = proc.stderr.read() if proc.stderr else ""
            rc = proc.wait()

    if stderr:
        print(stderr, file=sys.stderr, end="")
    if rc != 0:
        print(f"minidb exited with status {rc}", file=sys.stderr)
        return rc

    log_text = log_path.read_text(errors="replace")
    if "Error:" in log_text:
        print(f"load completed with SQL errors; inspect {log_path}", file=sys.stderr)
        for line in log_text.splitlines():
            if "Error:" in line:
                print(line, file=sys.stderr)
        return 1

    print(f"load complete; log={log_path}", file=sys.stderr)
    print(f"sample queries: {db_dir / 'perf_queries.sql'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())