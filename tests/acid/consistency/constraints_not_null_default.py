#!/usr/bin/env python3
"""Constraint enforcement test for ACID item C1.

Covers two related correctness properties at the DML layer:

  - NOT NULL violations on INSERT and UPDATE are surfaced as SQL errors,
    not silently dropped rows. Previously both executors would skip
    offending rows and report a smaller affected_rows count, masking
    constraint bugs in user code.

  - DEFAULT values are substituted when an INSERT column list omits a
    column. A NOT NULL column with a declared DEFAULT must accept inserts
    that do not mention it; a NOT NULL column without a default must
    still reject them.
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    minidb_query,
    run_minidb,
    temp_db,
)


def assert_contains(output: str, needle: str, ctx: str, seed: int) -> None:
    if needle not in output:
        raise AssertionError(f"{ctx} expected {needle!r} in output seed={seed}\n{output}")


def assert_not_contains(output: str, needle: str, ctx: str, seed: int) -> None:
    if needle in output:
        raise AssertionError(f"{ctx} did not expect {needle!r} in output seed={seed}\n{output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-c1.")
    try:
        # ---- INSERT NOT NULL is now a hard error -----------------------
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, name TEXT NOT NULL, tag TEXT);",
            "INSERT INTO t VALUES (1, 'alice', 'a');",
        ])
        assert_contains(setup, "1", "valid INSERT", seed)
        assert_not_contains(setup, "Error:", "valid INSERT", seed)

        bad_insert = run_minidb(args.bin, db, [
            "INSERT INTO t (id, tag) VALUES (2, 'b');",
        ])
        assert_contains(bad_insert, "Error", "INSERT missing NOT NULL", seed)

        rows = minidb_query(args.bin, db, "SELECT id, name FROM t ORDER BY id;", seed)
        if rows != [("1", "alice")]:
            raise AssertionError(
                f"NOT NULL violating row should not persist, got {rows} seed={seed}")

        # ---- UPDATE NOT NULL is also a hard error ----------------------
        bad_update = run_minidb(args.bin, db, [
            "UPDATE t SET name = NULL WHERE id = 1;",
        ])
        assert_contains(bad_update, "Error", "UPDATE setting NOT NULL to NULL", seed)
        rows = minidb_query(args.bin, db, "SELECT id, name FROM t ORDER BY id;", seed)
        if rows != [("1", "alice")]:
            raise AssertionError(
                f"row should be untouched after rejected UPDATE, got {rows} seed={seed}")

        # ---- DEFAULT substitution fills omitted columns ----------------
        cleanup(db)
        db2 = temp_db("minidb-c1-default.")
        try:
            out = run_minidb(args.bin, db2, [
                "CREATE TABLE u (id INT PRIMARY KEY, name TEXT NOT NULL DEFAULT 'anon', v INT DEFAULT 7);",
                "INSERT INTO u (id) VALUES (1);",
                "INSERT INTO u (id, name) VALUES (2, 'bob');",
                "INSERT INTO u (id, v) VALUES (3, 99);",
            ])
            assert_not_contains(out, "Error:", "INSERT relying on DEFAULT", seed)
            rows = minidb_query(args.bin, db2,
                                "SELECT id, name, v FROM u ORDER BY id;", seed)
            expected = [
                ("1", "anon", "7"),
                ("2", "bob", "7"),
                ("3", "anon", "99"),
            ]
            if rows != expected:
                raise AssertionError(
                    f"DEFAULT substitution mismatch seed={seed}\n"
                    f"actual={rows}\nexpected={expected}")
        finally:
            cleanup(db2)
        db = None

        print(f"constraints_not_null_default PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"constraints_not_null_default FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        if db:
            cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
