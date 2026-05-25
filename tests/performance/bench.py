#!/usr/bin/env python3
"""MiniDB performance benchmark — measures ops/sec for core operations."""

from __future__ import annotations
import argparse
import os
import subprocess
import sys
import tempfile
import shutil
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))


def run(bin_path: str, db_dir: str, sql_lines: list[str], timeout: int = 120) -> tuple[str, float]:
    """Run SQL statements, return (output, elapsed_seconds)."""
    if sql_lines[-1].strip().lower() != "exit":
        sql_lines.append("exit")
    start = time.monotonic()
    proc = subprocess.run(
        [bin_path, "--dir", db_dir],
        input="\n".join(sql_lines) + "\n",
        text=True,
        capture_output=True,
        timeout=timeout,
    )
    elapsed = time.monotonic() - start
    out = proc.stdout + proc.stderr
    return out, elapsed


def bench_bulk_insert(bin_path: str, db_dir: str, n: int = 5000, batch_size: int = 40) -> dict:
    """Benchmark INSERT throughput."""
    stmts = ["CREATE TABLE bench_ins (id INT PRIMARY KEY, val INT, pad TEXT);"]
    batch = []
    for i in range(1, n + 1):
        batch.append(f"({i},{i*10},'padding_text_{i:06d}')")
        if len(batch) >= batch_size:
            stmts.append(f"INSERT INTO bench_ins VALUES {','.join(batch)};")
            batch = []
    if batch:
        stmts.append(f"INSERT INTO bench_ins VALUES {','.join(batch)};")
    stmts.append(f"SELECT COUNT(*) FROM bench_ins;")
    out, elapsed = run(bin_path, db_dir, stmts)
    if "Error" in out and "already exists" not in out.lower():
        return {"name": "bulk_insert", "rows": n, "elapsed": elapsed, "ops_sec": 0, "error": out[:200]}
    return {"name": "bulk_insert", "rows": n, "elapsed": elapsed, "ops_sec": n / elapsed}


def bench_seq_scan(bin_path: str, db_dir: str, n_queries: int = 50) -> dict:
    """Benchmark sequential scan (full table scan with aggregation)."""
    stmts = []
    for _ in range(n_queries):
        stmts.append("SELECT COUNT(*) FROM bench_ins;")
    out, elapsed = run(bin_path, db_dir, stmts)
    if "Error" in out:
        return {"name": "seq_scan", "queries": n_queries, "elapsed": elapsed, "ops_sec": 0, "error": out[:200]}
    return {"name": "seq_scan", "queries": n_queries, "elapsed": elapsed, "ops_sec": n_queries / elapsed}


def bench_pk_lookup(bin_path: str, db_dir: str, n_queries: int = 200) -> dict:
    """Benchmark primary key point lookups (IndexScan)."""
    import random
    rng = random.Random(42)
    stmts = []
    for _ in range(n_queries):
        k = rng.randint(1, 5000)
        stmts.append(f"SELECT id, val FROM bench_ins WHERE id = {k};")
    out, elapsed = run(bin_path, db_dir, stmts)
    if "Error" in out:
        return {"name": "pk_lookup", "queries": n_queries, "elapsed": elapsed, "ops_sec": 0, "error": out[:200]}
    return {"name": "pk_lookup", "queries": n_queries, "elapsed": elapsed, "ops_sec": n_queries / elapsed}


def bench_range_scan(bin_path: str, db_dir: str, n_queries: int = 50) -> dict:
    """Benchmark range scans."""
    stmts = []
    for i in range(n_queries):
        lo = i * 100 + 1
        hi = lo + 99
        stmts.append(f"SELECT COUNT(*) FROM bench_ins WHERE id >= {lo} AND id <= {hi};")
    out, elapsed = run(bin_path, db_dir, stmts)
    if "Error" in out:
        return {"name": "range_scan", "queries": n_queries, "elapsed": elapsed, "ops_sec": 0, "error": out[:200]}
    return {"name": "range_scan", "queries": n_queries, "elapsed": elapsed, "ops_sec": n_queries / elapsed}


def bench_update(bin_path: str, db_dir: str, n: int = 500) -> dict:
    """Benchmark single-row UPDATE throughput."""
    import random
    rng = random.Random(43)
    stmts = []
    for _ in range(n):
        k = rng.randint(1, 5000)
        stmts.append(f"UPDATE bench_ins SET val = {rng.randint(1,99999)} WHERE id = {k};")
    out, elapsed = run(bin_path, db_dir, stmts)
    if "Error" in out and "statement timeout" not in out.lower():
        return {"name": "update", "ops": n, "elapsed": elapsed, "ops_sec": 0, "error": out[:200]}
    return {"name": "update", "ops": n, "elapsed": elapsed, "ops_sec": n / elapsed}


def bench_delete_insert(bin_path: str, db_dir: str, n: int = 300) -> dict:
    """Benchmark DELETE + re-INSERT cycle."""
    stmts = []
    for i in range(1, n + 1):
        stmts.append(f"DELETE FROM bench_ins WHERE id = {i};")
        stmts.append(f"INSERT INTO bench_ins VALUES ({i},{i*100},'re_inserted_{i:06d}');")
    out, elapsed = run(bin_path, db_dir, stmts)
    if "Error" in out and "statement timeout" not in out.lower():
        return {"name": "delete_insert", "ops": n * 2, "elapsed": elapsed, "ops_sec": 0, "error": out[:200]}
    return {"name": "delete_insert", "ops": n * 2, "elapsed": elapsed, "ops_sec": (n * 2) / elapsed}


def bench_join(bin_path: str, db_dir: str, n_queries: int = 20) -> dict:
    """Benchmark self-join queries."""
    stmts = []
    for _ in range(n_queries):
        stmts.append("SELECT COUNT(*) FROM bench_ins a INNER JOIN bench_ins b ON a.val = b.val WHERE a.id <= 100 AND b.id <= 100;")
    out, elapsed = run(bin_path, db_dir, stmts)
    if "Error" in out:
        return {"name": "join", "queries": n_queries, "elapsed": elapsed, "ops_sec": 0, "error": out[:200]}
    return {"name": "join", "queries": n_queries, "elapsed": elapsed, "ops_sec": n_queries / elapsed}


def bench_txn_batch(bin_path: str, db_dir: str, n: int = 500, batch: int = 50) -> dict:
    """Benchmark explicit transaction batches (BEGIN + N inserts + COMMIT)."""
    stmts = ["CREATE TABLE bench_txn (id INT, val INT);"]
    row_id = 0
    txn_count = n // batch
    for _ in range(txn_count):
        stmts.append("BEGIN;")
        for _ in range(batch):
            row_id += 1
            stmts.append(f"INSERT INTO bench_txn VALUES ({row_id},{row_id*10});")
        stmts.append("COMMIT;")
    stmts.append("SELECT COUNT(*) FROM bench_txn;")
    out, elapsed = run(bin_path, db_dir, stmts)
    if "Error" in out and "already exists" not in out.lower():
        return {"name": "txn_batch", "rows": row_id, "elapsed": elapsed, "ops_sec": 0, "error": out[:200]}
    return {"name": "txn_batch", "rows": row_id, "elapsed": elapsed, "ops_sec": row_id / elapsed}


def bench_vacuum(bin_path: str, db_dir: str) -> dict:
    """Benchmark VACUUM on a table with dead tuples."""
    stmts = [
        "DELETE FROM bench_ins WHERE id <= 2000;",
        "VACUUM;",
        "SELECT COUNT(*) FROM bench_ins;",
    ]
    out, elapsed = run(bin_path, db_dir, stmts)
    return {"name": "vacuum", "elapsed": elapsed, "note": "delete 2000 + vacuum"}


def main() -> int:
    parser = argparse.ArgumentParser(description="MiniDB Performance Benchmark")
    parser.add_argument("bin", nargs="?", default="./build/minidb")
    parser.add_argument("--quick", action="store_true", help="Reduced workload for fast iteration")
    args = parser.parse_args()

    db_dir = tempfile.mkdtemp(prefix="minidb-bench.")

    # Write config for benchmark — use production-realistic defaults.
    # gc_ops_threshold=10000 matches the default; lower values cause
    # frequent GC scans that dominate autocommit write latency.
    with open(os.path.join(db_dir, "minidb.conf"), "w") as f:
        f.write("""shared_buffers=4MB
work_mem=2MB
enable_indexscan=on
enable_indexonlyscan=on
wal_fsync=on
gc_enabled=on
gc_ops_threshold=10000
statement_timeout_ms=30000
""")

    scale = 1 if not args.quick else 4

    print("=" * 60)
    print("MiniDB Performance Benchmark")
    print("=" * 60)

    results = []

    # 1. Bulk INSERT
    r = bench_bulk_insert(args.bin, db_dir, n=5000 // scale)
    results.append(r)
    print(f"  {r['name']:20s}  {r['rows']:>6} rows  {r['elapsed']:7.2f}s  {r['ops_sec']:>8.0f} rows/s")

    # Collect statistics so the optimizer uses IndexScan where profitable.
    run(args.bin, db_dir, ["ANALYZE bench_ins;"])

    # 2. SeqScan
    r = bench_seq_scan(args.bin, db_dir, n_queries=50 // scale)
    results.append(r)
    print(f"  {r['name']:20s}  {r['queries']:>6} q     {r['elapsed']:7.2f}s  {r['ops_sec']:>8.1f} q/s")

    # 3. PK point lookup
    r = bench_pk_lookup(args.bin, db_dir, n_queries=200 // scale)
    results.append(r)
    print(f"  {r['name']:20s}  {r['queries']:>6} q     {r['elapsed']:7.2f}s  {r['ops_sec']:>8.1f} q/s")

    # 4. Range scan
    r = bench_range_scan(args.bin, db_dir, n_queries=50 // scale)
    results.append(r)
    print(f"  {r['name']:20s}  {r['queries']:>6} q     {r['elapsed']:7.2f}s  {r['ops_sec']:>8.1f} q/s")

    # 5. UPDATE
    r = bench_update(args.bin, db_dir, n=500 // scale)
    results.append(r)
    print(f"  {r['name']:20s}  {r['ops']:>6} ops   {r['elapsed']:7.2f}s  {r['ops_sec']:>8.1f} ops/s")

    # 6. DELETE + re-INSERT
    r = bench_delete_insert(args.bin, db_dir, n=300 // scale)
    results.append(r)
    print(f"  {r['name']:20s}  {r['ops']:>6} ops   {r['elapsed']:7.2f}s  {r['ops_sec']:>8.1f} ops/s")

    # 7. JOIN
    r = bench_join(args.bin, db_dir, n_queries=20 // scale)
    results.append(r)
    print(f"  {r['name']:20s}  {r['queries']:>6} q     {r['elapsed']:7.2f}s  {r['ops_sec']:>8.1f} q/s")

    # 8. Txn batch
    r = bench_txn_batch(args.bin, db_dir, n=500 // scale, batch=50)
    results.append(r)
    print(f"  {r['name']:20s}  {r['rows']:>6} rows  {r['elapsed']:7.2f}s  {r['ops_sec']:>8.0f} rows/s")

    # 9. VACUUM
    r = bench_vacuum(args.bin, db_dir)
    results.append(r)
    print(f"  {r['name']:20s}  {r.get('note',''):>20s}  {r['elapsed']:7.2f}s")

    print("=" * 60)

    # Check for errors
    errs = [r for r in results if "error" in r]
    if errs:
        print("\nErrors:")
        for e in errs:
            print(f"  {e['name']}: {e['error']}")
        return 1

    shutil.rmtree(db_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
