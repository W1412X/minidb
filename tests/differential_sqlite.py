#!/usr/bin/env python3
"""Differential SQL testing against SQLite for the supported SQL subset."""

from __future__ import annotations

import argparse
import os
import sqlite3
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    assert_no_error,
    assert_rows_equal,
    cleanup,
    minidb_query,
    run_minidb,
    seeded_rng,
    sqlite_rows,
    temp_db,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    rng = seeded_rng(seed)
    cases = args.cases or (2000 if args.stress else 160)
    db_dir = temp_db("minidb-diff.")
    conn = sqlite3.connect(":memory:")
    try:
        ddl = [
            "CREATE TABLE a (id INT PRIMARY KEY, x INT, y TEXT);",
            "CREATE TABLE b (id INT PRIMARY KEY, a_id INT, z INT);",
        ]
        for sql in ddl:
            conn.execute(sql)
        setup = list(ddl)
        for i in range(1, 41):
            x = (i * 7) % 13
            y = f"v{i % 6}"
            sql = f"INSERT INTO a VALUES ({i}, {x}, '{y}');"
            setup.append(sql)
            conn.execute(sql)
        bid = 1
        for a_id in range(1, 41):
            for k in range(a_id % 3):
                z = (a_id + k * 5) % 17
                sql = f"INSERT INTO b VALUES ({bid}, {a_id}, {z});"
                setup.append(sql)
                conn.execute(sql)
                bid += 1
        conn.commit()
        output = run_minidb(args.bin, db_dir, setup, timeout=60)
        assert_no_error(output, seed, "differential setup")

        fixed = [
            "SELECT COUNT(*) FROM a;",
            "SELECT id, x FROM a WHERE x BETWEEN 3 AND 7 ORDER BY x, id LIMIT 8;",
            "SELECT DISTINCT y FROM a ORDER BY y;",
            "SELECT a.id, b.z FROM a JOIN b ON a.id=b.a_id WHERE b.z > 8 ORDER BY a.id, b.z LIMIT 10;",
            "SELECT a.id FROM a LEFT JOIN b ON a.id=b.a_id WHERE b.id IS NULL ORDER BY a.id;",
            "SELECT a_id, COUNT(*), MIN(z), MAX(z) FROM b GROUP BY a_id HAVING COUNT(*) > 1 ORDER BY a_id LIMIT 6;",
            "SELECT id FROM a WHERE id IN (SELECT a_id FROM b WHERE z < 4) ORDER BY id LIMIT 10;",
        ]
        generated = []
        predicates = [
            "x = {n}",
            "x != {n}",
            "x < {n}",
            "x <= {n}",
            "x > {n}",
            "x >= {n}",
            "id BETWEEN {lo} AND {hi}",
            "y = 'v{m}'",
            "y LIKE 'v%'",
        ]
        orderings = ["id ASC", "id DESC", "x ASC, id ASC", "y ASC, id ASC"]
        for _ in range(cases):
            kind = rng.choice(["scan", "agg", "join", "left", "in"])
            n = rng.randint(0, 14)
            lo = rng.randint(1, 30)
            hi = rng.randint(lo, 45)
            m = rng.randint(0, 5)
            pred = rng.choice(predicates).format(n=n, lo=lo, hi=hi, m=m)
            limit = rng.randint(1, 12)
            if kind == "scan":
                generated.append(f"SELECT id, x, y FROM a WHERE {pred} ORDER BY {rng.choice(orderings)} LIMIT {limit};")
            elif kind == "agg":
                generated.append(f"SELECT x, COUNT(*) FROM a WHERE id BETWEEN {lo} AND {hi} GROUP BY x ORDER BY x LIMIT {limit};")
            elif kind == "join":
                generated.append(f"SELECT a.id, b.z FROM a JOIN b ON a.id=b.a_id WHERE a.{pred} ORDER BY a.id, b.z LIMIT {limit};")
            elif kind == "left":
                generated.append(f"SELECT a.id, b.z FROM a LEFT JOIN b ON a.id=b.a_id WHERE a.id BETWEEN {lo} AND {hi} ORDER BY a.id, b.z LIMIT {limit};")
            else:
                generated.append(f"SELECT id FROM a WHERE id IN (SELECT a_id FROM b WHERE z <= {n}) ORDER BY id LIMIT {limit};")

        for idx, sql in enumerate(fixed + generated):
            actual = minidb_query(args.bin, db_dir, sql, seed, timeout=30)
            expected = sqlite_rows(conn, sql)
            assert_rows_equal(actual, expected, seed, f"case={idx} {sql}")

        print(f"differential_sqlite PASS seed={seed} cases={len(fixed) + len(generated)}")
        return 0
    except Exception as exc:
        print(f"differential_sqlite FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        conn.close()
        cleanup(db_dir)


if __name__ == "__main__":
    raise SystemExit(main())

