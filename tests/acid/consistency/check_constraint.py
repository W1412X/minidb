#!/usr/bin/env python3
"""Column-level CHECK constraint enforcement (ACID B3).

Before this work CREATE TABLE silently accepted `CHECK (...)` clauses but
never enforced them. The parser now captures the predicate text, the
catalog stores it through restart, and InsertExecutor / UpdateExecutor
re-parse and evaluate it against every candidate row.

The test pins:

  1. INSERT of a row that satisfies the CHECK is accepted; a row that
     violates it is rejected with a "CHECK constraint violated" error and
     does NOT appear in the table.
  2. UPDATE that would push a row over the CHECK is rejected; the row
     keeps its old value.
  3. NULL on the CHECK target column passes (UNKNOWN is permitted per
     SQL standard / PostgreSQL semantics) unless NOT NULL forbids it.
  4. The CHECK predicate survives a database restart — it lives in the
     schema serialisation, not just in parser memory.
  5. Multi-column predicates that reference other columns work
     (e.g. `CHECK (a < b)`).
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


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-check.")
    try:
        # ---- CREATE TABLE with CHECK ---------------------------------
        out = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT, age INT CHECK (age >= 0), "
            "lo INT, hi INT CHECK (hi > lo));",
            "INSERT INTO t VALUES (1, 20, 1, 5);",
            "INSERT INTO t VALUES (2, 0, 0, 1);",   # boundary: age=0 ok, hi>lo ok
        ])
        if "Error" in out:
            raise AssertionError(f"valid inserts rejected: {out}")

        # ---- violating INSERT must be rejected -----------------------
        bad = run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (3, -1, 0, 1);",
        ])
        if "CHECK constraint violated" not in bad:
            raise AssertionError(f"expected CHECK error on INSERT, got: {bad}")
        rows = minidb_query(args.bin, db, "SELECT id FROM t ORDER BY id;", seed)
        if rows != [("1",), ("2",)]:
            raise AssertionError(f"INSERT-on-CHECK leaked row: {rows}")

        # ---- cross-column CHECK ---------------------------------------
        bad2 = run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (4, 5, 10, 10);",   # hi NOT > lo
        ])
        if "CHECK constraint violated" not in bad2:
            raise AssertionError(f"expected cross-col CHECK error, got: {bad2}")

        # ---- violating UPDATE must be rejected ------------------------
        bad_upd = run_minidb(args.bin, db, [
            "UPDATE t SET age = -5 WHERE id = 1;",
        ])
        if "CHECK constraint violated" not in bad_upd:
            raise AssertionError(f"expected CHECK error on UPDATE, got: {bad_upd}")
        rows = minidb_query(args.bin, db,
                            "SELECT id, age FROM t WHERE id = 1;", seed)
        if rows != [("1", "20")]:
            raise AssertionError(f"UPDATE-on-CHECK mutated row: {rows}")

        # ---- NULL passes CHECK (UNKNOWN result is permitted) ----------
        out_null = run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (5, NULL, NULL, NULL);",
        ])
        if "Error" in out_null:
            raise AssertionError(f"NULL should pass CHECK: {out_null}")

        # ---- CHECK predicate persists across restart ------------------
        # The schema serialisation now carries check_expr, so the next
        # process must still reject violators.
        out_after = run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (6, -42, 0, 1);",
        ])
        if "CHECK constraint violated" not in out_after:
            raise AssertionError(f"CHECK lost after restart: {out_after}")

        rows = minidb_query(args.bin, db, "SELECT id FROM t ORDER BY id;", seed)
        if rows != [("1",), ("2",), ("5",)]:
            raise AssertionError(f"final row set wrong: {rows}")

        print(f"check_constraint PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"check_constraint FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
