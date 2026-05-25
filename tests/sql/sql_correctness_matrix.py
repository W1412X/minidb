#!/usr/bin/env python3
"""Deterministic SQL correctness matrix.

Covers hand-written golden cases for SQL semantics, joins, aggregation,
subqueries, index-visible updates/deletes, MVCC basics, and spill paths.
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    assert_no_error,
    assert_rows_equal,
    cleanup,
    minidb_query,
    run_minidb,
    seeded_rng,
    temp_db,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    rng = seeded_rng(seed)
    cases = args.cases or (450 if args.stress else 90)
    db_dir = temp_db("minidb-sql-matrix.")
    try:
        setup = [
            "CREATE TABLE users (id INT PRIMARY KEY, age INT, score INT, name TEXT, tag TEXT);",
            "CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, amount INT, status TEXT);",
            "CREATE TABLE empty_t (id INT, v INT);",
            "CREATE INDEX idx_users_age ON users(age);",
            "CREATE INDEX idx_orders_user ON orders(user_id);",
        ]
        for i in range(1, 31):
            name = f"name{i}"
            tag = "NULL" if i % 7 == 0 else f"'tag{i % 5}'"
            score = "NULL" if i % 6 == 0 else str((i * 11) % 100)
            setup.append(
                f"INSERT INTO users VALUES ({i}, {18 + (i % 9)}, {score}, '{name}', {tag});"
            )
        oid = 1
        for uid in range(1, 31):
            for k in range(uid % 4):
                setup.append(
                    f"INSERT INTO orders VALUES ({oid}, {uid}, {10 + uid * k}, "
                    f"'{'paid' if k % 2 == 0 else 'open'}');"
                )
                oid += 1
        out = run_minidb(args.bin, db_dir, setup, timeout=60)
        assert_no_error(out, seed, "setup")

        expected_queries = [
            ("SELECT COUNT(*) FROM users;", [("30",)]),
            ("SELECT COUNT(score) FROM users;", [("25",)]),
            ("SELECT id FROM users WHERE id = 3;", [("3",)]),
            ("SELECT id FROM users WHERE id != 3 ORDER BY id LIMIT 3;", [("1",), ("2",), ("4",)]),
            ("SELECT id FROM users WHERE age >= 20 AND age <= 22 ORDER BY id LIMIT 4;", [("2",), ("3",), ("4",), ("11",)]),
            ("SELECT id FROM users WHERE NOT (age < 20) ORDER BY id LIMIT 3;", [("2",), ("3",), ("4",)]),
            ("SELECT id FROM users WHERE tag IS NULL ORDER BY id;", [("7",), ("14",), ("21",), ("28",)]),
            ("SELECT id FROM users WHERE tag IS NOT NULL ORDER BY id LIMIT 3;", [("1",), ("2",), ("3",)]),
            ("SELECT id FROM users WHERE name LIKE 'name1%' ORDER BY id LIMIT 4;", [("1",), ("10",), ("11",), ("12",)]),
            ("SELECT id FROM users WHERE name LIKE '%5' ORDER BY id;", [("5",), ("15",), ("25",)]),
            ("SELECT id FROM users WHERE id BETWEEN 3 AND 5 ORDER BY id;", [("3",), ("4",), ("5",)]),
            ("SELECT id FROM users WHERE id IN (1, 4, 7) ORDER BY id;", [("1",), ("4",), ("7",)]),
            ("SELECT id FROM users WHERE id NOT IN (1, 2, 3) ORDER BY id LIMIT 3;", [("4",), ("5",), ("6",)]),
            ("SELECT DISTINCT tag FROM users WHERE tag IS NOT NULL ORDER BY tag LIMIT 3;", [("tag0",), ("tag1",), ("tag2",)]),
            ("SELECT id FROM users ORDER BY age DESC, id ASC LIMIT 3;", [("8",), ("17",), ("26",)]),
            ("SELECT id FROM users ORDER BY id LIMIT 3 OFFSET 2;", [("3",), ("4",), ("5",)]),
            ("SELECT COUNT(*) FROM users u INNER JOIN orders o ON u.id=o.user_id;", [("45",)]),
            ("SELECT COUNT(*) FROM users u LEFT JOIN orders o ON u.id=o.user_id WHERE o.id IS NULL;", [("7",)]),
            ("SELECT COUNT(*) FROM users CROSS JOIN empty_t;", [("0",)]),
            ("SELECT user_id, COUNT(*) FROM orders GROUP BY user_id HAVING COUNT(*) > 2 ORDER BY user_id LIMIT 3;", [("3", "3"), ("7", "3"), ("11", "3")]),
            ("SELECT MIN(amount), MAX(amount), SUM(amount) FROM orders WHERE user_id = 3;", [("10", "16", "39")]),
            ("SELECT COUNT(*) FROM users WHERE id IN (SELECT user_id FROM orders WHERE amount > 20);", [("11",)]),
            ("SELECT COUNT(*) FROM empty_t;", [("0",)]),
            ("SELECT COUNT(v), SUM(v), MIN(v), MAX(v) FROM empty_t;", [("0", "NULL", "NULL", "NULL")]),
        ]
        for sql, expected in expected_queries:
            assert_rows_equal(minidb_query(args.bin, db_dir, sql, seed), expected, seed, sql)

        txn = run_minidb(
            args.bin,
            db_dir,
            [
                "BEGIN;",
                "INSERT INTO users VALUES (100, 40, 50, 'rollback', 'txn');",
                "UPDATE users SET age = 99 WHERE id = 1;",
                "DELETE FROM users WHERE id = 2;",
                "ROLLBACK;",
                "SELECT id, age FROM users WHERE id IN (1,2,100) ORDER BY id;",
                "BEGIN;",
                "INSERT INTO users VALUES (101, 41, 51, 'commit', 'txn');",
                "COMMIT;",
                "SELECT id FROM users WHERE id = 101;",
            ],
            timeout=30,
        )
        assert_no_error(txn, seed, "transaction matrix")
        assert "1 | 19" in txn and "2 | 20" in txn and "100 |" not in txn and "\n101\n" in txn

        # Random index/update/delete checks against an in-memory model.
        model = {i: i * 10 for i in range(1, 51)}
        stmts = ["CREATE TABLE kv (id INT PRIMARY KEY, v INT);", "CREATE INDEX idx_kv_v ON kv(v);"]
        stmts.extend(f"INSERT INTO kv VALUES ({i}, {v});" for i, v in model.items())
        out = run_minidb(args.bin, db_dir, stmts, timeout=60)
        assert_no_error(out, seed, "kv setup")
        for step in range(cases):
            key = rng.randint(1, 75)
            op = rng.choice(["insert", "update", "delete", "select"])
            if op == "insert":
                val = rng.randint(0, 1000)
                sql = f"INSERT INTO kv VALUES ({key}, {val});"
                if key not in model:
                    model[key] = val
            elif op == "update":
                val = rng.randint(0, 1000)
                sql = f"UPDATE kv SET v = {val} WHERE id = {key};"
                if key in model:
                    model[key] = val
            elif op == "delete":
                sql = f"DELETE FROM kv WHERE id = {key};"
                model.pop(key, None)
            else:
                sql = f"SELECT id, v FROM kv WHERE id = {key};"
            out = run_minidb(args.bin, db_dir, [sql], timeout=20)
            assert_no_error(out, seed, f"random step={step} sql={sql}")
            if step % 10 == 0 or step == cases - 1:
                expected = [(str(k), str(model[k])) for k in sorted(model)[:20]]
                actual = minidb_query(args.bin, db_dir, "SELECT id, v FROM kv ORDER BY id LIMIT 20;", seed)
                assert_rows_equal(actual, expected, seed, f"kv full check step={step}")

        print(f"sql_correctness_matrix PASS seed={seed} cases={cases}")
        return 0
    except Exception as exc:
        print(f"sql_correctness_matrix FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db_dir)


if __name__ == "__main__":
    raise SystemExit(main())
