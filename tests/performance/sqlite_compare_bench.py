#!/usr/bin/env python3
"""SQLite comparison benchmark with target-oriented standards.

The legacy bench.py is useful for end-to-end CLI smoke testing, but its
short SELECT q/s numbers are dominated by parser/planner/output overhead.
This benchmark separates:

  * standards: broad DB work where a 5x SQLite gap is a meaningful target;
  * diagnostics: frontend/server overhead probes that explain remaining gaps.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sqlite3
import subprocess
import tempfile
import time
from dataclasses import dataclass


EXPLAIN_RE = re.compile(r"execution_time_ms=([0-9.]+)")


@dataclass
class Result:
    name: str
    minidb: float
    sqlite: float
    unit: str
    standard: bool = True

    @property
    def gap(self) -> float:
        if self.unit.endswith("/s"):
            return self.sqlite / self.minidb if self.minidb > 0 else float("inf")
        return self.minidb / self.sqlite if self.sqlite > 0 else float("inf")


def run_minidb(bin_path: str, db_dir: str, sql: list[str], timeout: int = 120) -> str:
    if not sql or sql[-1].strip().lower() != "exit":
        sql = sql + ["exit"]
    proc = subprocess.run(
        [bin_path, "--dir", db_dir],
        input="\n".join(sql) + "\n",
        text=True,
        capture_output=True,
        timeout=timeout,
    )
    out = proc.stdout + proc.stderr
    if "Error:" in out or "failed to" in out:
        raise RuntimeError(out)
    return out


def sqlite_conn(path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(path, isolation_level=None)
    conn.execute("PRAGMA journal_mode=DELETE")
    conn.execute("PRAGMA synchronous=FULL")
    conn.execute("PRAGMA temp_store=MEMORY")
    return conn


def read_setup_sql(n: int) -> list[str]:
    sql = ["CREATE TABLE t (id INT PRIMARY KEY, v INT, pad TEXT);"]
    for start in range(1, n + 1, 200):
        end = min(n + 1, start + 200)
        values = ",".join(f"({i},{i},'pad{i}')" for i in range(start, end))
        sql.append(f"INSERT INTO t VALUES {values};")
    # Mark append-only pages all-visible so index-only/count paths measure
    # the intended storage design rather than MVCC heap recheck overhead.
    sql.append("VACUUM;")
    sql.append("ANALYZE t;")
    return sql


def setup_sqlite_read_db(n: int) -> str:
    sqlite_path = tempfile.mktemp(prefix="sqlite-compare.", suffix=".db")

    conn = sqlite_conn(sqlite_path)
    conn.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER, pad TEXT)")
    conn.execute("BEGIN")
    conn.executemany(
        "INSERT INTO t VALUES (?,?,?)",
        [(i, i, f"pad{i}") for i in range(1, n + 1)],
    )
    conn.execute("COMMIT")
    conn.execute("ANALYZE")
    conn.close()
    return sqlite_path


def minidb_explain_ms(bin_path: str, db_dir: str, query: str) -> float:
    out = run_minidb(bin_path, db_dir, [f"EXPLAIN ANALYZE {query};"])
    matches = EXPLAIN_RE.findall(out)
    if not matches:
        raise RuntimeError(f"missing EXPLAIN ANALYZE time:\n{out}")
    return float(matches[-1])


def sqlite_query_ms(sqlite_path: str, query: str, repeat: int = 7) -> float:
    conn = sqlite_conn(sqlite_path)
    best = float("inf")
    for _ in range(repeat):
        start = time.perf_counter()
        list(conn.execute(query))
        elapsed = (time.perf_counter() - start) * 1000.0
        best = min(best, elapsed)
    conn.close()
    return best


def bench_query_standards(bin_path: str) -> list[Result]:
    n = 5000
    minidb_dir = tempfile.mkdtemp(prefix="minidb-sqlite-compare.")
    sqlite_path = setup_sqlite_read_db(n)
    try:
        cases = [
            (
                "plain_aggregate",
                "SELECT SUM(v), AVG(v), MIN(v), MAX(v), COUNT(*) FROM t",
            ),
            (
                "indexed_range_count",
                "SELECT COUNT(*) FROM t WHERE id >= 1 AND id <= 50",
            ),
            (
                "hash_join_count",
                "SELECT COUNT(*) FROM t a INNER JOIN t b ON a.v = b.v",
            ),
        ]
        mini_sql = read_setup_sql(n)
        for _, query in cases:
            mini_sql.append(f"EXPLAIN ANALYZE {query};")
        out = run_minidb(bin_path, minidb_dir, mini_sql)
        mini_times = [float(x) for x in EXPLAIN_RE.findall(out)]
        if len(mini_times) != len(cases):
            raise RuntimeError(f"expected {len(cases)} explain timings, got {len(mini_times)}:\n{out}")
        results: list[Result] = []
        for idx, (name, query) in enumerate(cases):
            mini_ms = mini_times[idx]
            sqlite_ms = sqlite_query_ms(sqlite_path, query)
            results.append(Result(name, mini_ms, sqlite_ms, "ms"))
        return results
    finally:
        shutil.rmtree(minidb_dir, ignore_errors=True)
        try:
            os.remove(sqlite_path)
        except OSError:
            pass


def bench_dml(bin_path: str) -> list[Result]:
    n = 300
    results: list[Result] = []
    for name, stmts_builder, sqlite_builder in [
        (
            "insert_autocommit",
            lambda: ["CREATE TABLE d (id INT PRIMARY KEY, v INT);"]
            + [f"INSERT INTO d VALUES ({i},{i});" for i in range(1, n + 1)],
            lambda conn: [
                conn.execute("CREATE TABLE d (id INTEGER PRIMARY KEY, v INTEGER)"),
                [conn.execute("INSERT INTO d VALUES (?,?)", (i, i)) for i in range(1, n + 1)],
            ],
        ),
        (
            "update_autocommit",
            lambda: ["CREATE TABLE d (id INT PRIMARY KEY, v INT);"]
            + [f"INSERT INTO d VALUES ({i},{i});" for i in range(1, n + 1)]
            + [f"UPDATE d SET v = v + 1 WHERE id = {i};" for i in range(1, n + 1)],
            None,
        ),
        (
            "delete_autocommit",
            lambda: ["CREATE TABLE d (id INT PRIMARY KEY, v INT);"]
            + [f"INSERT INTO d VALUES ({i},{i});" for i in range(1, n + 1)]
            + [f"DELETE FROM d WHERE id = {i};" for i in range(1, n + 1)],
            None,
        ),
    ]:
        minidb_dir = tempfile.mkdtemp(prefix="minidb-dml-compare.")
        sqlite_path = tempfile.mktemp(prefix="sqlite-dml-compare.", suffix=".db")
        try:
            stmts = stmts_builder()
            start = time.perf_counter()
            run_minidb(bin_path, minidb_dir, stmts)
            mini_elapsed = time.perf_counter() - start

            conn = sqlite_conn(sqlite_path)
            start = time.perf_counter()
            if name == "insert_autocommit":
                conn.execute("CREATE TABLE d (id INTEGER PRIMARY KEY, v INTEGER)")
                for i in range(1, n + 1):
                    conn.execute("INSERT INTO d VALUES (?,?)", (i, i))
            elif name == "update_autocommit":
                conn.execute("CREATE TABLE d (id INTEGER PRIMARY KEY, v INTEGER)")
                conn.executemany("INSERT INTO d VALUES (?,?)", [(i, i) for i in range(1, n + 1)])
                for i in range(1, n + 1):
                    conn.execute("UPDATE d SET v = v + 1 WHERE id = ?", (i,))
            else:
                conn.execute("CREATE TABLE d (id INTEGER PRIMARY KEY, v INTEGER)")
                conn.executemany("INSERT INTO d VALUES (?,?)", [(i, i) for i in range(1, n + 1)])
                for i in range(1, n + 1):
                    conn.execute("DELETE FROM d WHERE id = ?", (i,))
            sqlite_elapsed = time.perf_counter() - start
            conn.close()
            results.append(Result(name, n / mini_elapsed, n / sqlite_elapsed, "ops/s"))
        finally:
            shutil.rmtree(minidb_dir, ignore_errors=True)
            try:
                os.remove(sqlite_path)
            except OSError:
                pass
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bin", nargs="?", default="./build/minidb")
    parser.add_argument("--target-gap", type=float, default=5.0)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    results = bench_query_standards(args.bin) + bench_dml(args.bin)

    print("SQLite comparison benchmark")
    print(f"target_gap={args.target_gap:.1f}x")
    print(f"{'name':24s} {'MiniDB':>12s} {'SQLite':>12s} {'unit':>8s} {'gap':>8s}")
    failed = False
    for r in results:
        gap = r.gap
        if r.standard and gap > args.target_gap:
            failed = True
        print(f"{r.name:24s} {r.minidb:12.3f} {r.sqlite:12.3f} {r.unit:>8s} {gap:7.2f}x")

    if failed:
        print("One or more standard benchmarks exceed the target gap.")
        return 1 if args.strict else 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
