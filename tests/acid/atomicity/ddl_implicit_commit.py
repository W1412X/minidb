#!/usr/bin/env python3
"""Transactional DDL: CREATE/DROP TABLE/INDEX participate in transactions.

MiniDB now supports PostgreSQL-style transactional DDL: schema changes
inside BEGIN..COMMIT/ROLLBACK are part of the transaction and can be
undone on rollback.

The test pins:
  1. CREATE TABLE inside a transaction is rolled back: table does not exist.
  2. DROP TABLE inside a transaction is rolled back: table still exists with data.
  3. CREATE INDEX inside a transaction is rolled back: index does not exist.
  4. INSERT + CREATE TABLE interleaved, then ROLLBACK: both undone.
  5. DDL outside a transaction still works (auto-committed).
  6. ALTER TABLE DROP COLUMN still does implicit commit (heap rewrite is irreversible).
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
    db = temp_db("minidb-ddl-txn.")
    try:
        # Set up an existing table.
        run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT);",
            "INSERT INTO t VALUES (1);",
        ])

        # ---- 1. CREATE TABLE + ROLLBACK → table does not exist ----------
        out = run_minidb(args.bin, db, [
            "BEGIN;",
            "CREATE TABLE foo (x INT);",
            "INSERT INTO foo VALUES (42);",
            "ROLLBACK;",
        ])
        # After rollback, foo should not exist.
        rows_check = run_minidb(args.bin, db, ["SELECT * FROM foo;"])
        if "Error" not in rows_check and "not found" not in rows_check:
            raise AssertionError(
                f"CREATE TABLE was not rolled back: {rows_check}")

        # ---- 2. DROP TABLE + ROLLBACK → table still exists ---------------
        out2 = run_minidb(args.bin, db, [
            "BEGIN;",
            "DROP TABLE t;",
            "ROLLBACK;",
        ])
        rows = minidb_query(args.bin, db, "SELECT id FROM t ORDER BY id;", seed)
        if rows != [("1",)]:
            raise AssertionError(
                f"DROP TABLE rollback lost data: {rows}")

        # ---- 3. CREATE INDEX + ROLLBACK → index does not exist -----------
        run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (2);",
        ])
        out3 = run_minidb(args.bin, db, [
            "BEGIN;",
            "CREATE INDEX idx_t_id ON t (id);",
            "ROLLBACK;",
        ])
        # The index should not exist after rollback. Querying is hard to
        # test directly, but we can try to create the same index again —
        # it should succeed because the first one was rolled back.
        out4 = run_minidb(args.bin, db, [
            "CREATE INDEX idx_t_id ON t (id);",
        ])
        if "Error" in out4:
            raise AssertionError(
                f"CREATE INDEX was not rolled back — could not re-create: {out4}")
        # Clean up the index we just created.
        run_minidb(args.bin, db, ["DROP INDEX idx_t_id;"])

        # ---- 4. INSERT + CREATE TABLE interleaved + ROLLBACK → both undone
        out5 = run_minidb(args.bin, db, [
            "BEGIN;",
            "INSERT INTO t VALUES (99);",
            "CREATE TABLE bar (y INT);",
            "INSERT INTO bar VALUES (7);",
            "ROLLBACK;",
        ])
        # INSERT into t should be rolled back
        rows = minidb_query(args.bin, db, "SELECT id FROM t ORDER BY id;", seed)
        if ("99",) in rows:
            raise AssertionError(
                f"INSERT was not rolled back with DDL: {rows}")
        # bar should not exist
        bar_check = run_minidb(args.bin, db, ["SELECT * FROM bar;"])
        if "Error" not in bar_check and "not found" not in bar_check:
            raise AssertionError(
                f"CREATE TABLE bar was not rolled back: {bar_check}")

        # ---- 5. DDL outside a transaction still works --------------------
        out6 = run_minidb(args.bin, db, ["CREATE TABLE baz (z INT);"])
        if "Error" in out6:
            raise AssertionError(f"plain DDL rejected: {out6}")
        baz_check = run_minidb(args.bin, db, ["SELECT * FROM baz;"])
        if "Error" in baz_check:
            raise AssertionError(f"baz table not accessible: {baz_check}")

        # ---- 6. ALTER TABLE DROP COLUMN still does implicit commit -------
        run_minidb(args.bin, db, [
            "ALTER TABLE baz ADD COLUMN a INT;",
            "ALTER TABLE baz ADD COLUMN b INT;",
        ])
        out7 = run_minidb(args.bin, db, [
            "BEGIN;",
            "INSERT INTO baz VALUES (1, 10, 20);",
            "ALTER TABLE baz DROP COLUMN b;",   # implicit commit here
            "ROLLBACK;",                         # no active txn -> error msg
        ])
        if "no active transaction" not in out7:
            raise AssertionError(
                f"ALTER DROP COLUMN should still do implicit commit: {out7}")
        # The INSERT committed via implicit commit.
        rows = minidb_query(args.bin, db, "SELECT z FROM baz;", seed)
        if rows != [("1",)]:
            raise AssertionError(
                f"pre-DDL INSERT was lost after implicit commit: {rows}")

        print(f"ddl_implicit_commit PASS seed={seed} "
              f"(transactional DDL: CREATE/DROP rollback works)")
        return 0
    except Exception as exc:
        print(f"ddl_implicit_commit FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
