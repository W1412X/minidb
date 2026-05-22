#!/usr/bin/env python3
"""Transactional DDL: all DDL operations participate in transactions.

MiniDB supports PostgreSQL-style transactional DDL: schema changes
inside BEGIN..COMMIT/ROLLBACK are part of the transaction and can be
undone on rollback.

ALTER TABLE DROP COLUMN uses metadata-only deletion (is_dropped flag)
like PostgreSQL — no heap rewrite, O(1), fully transactional.

The test pins:
  1. CREATE TABLE inside a transaction is rolled back: table does not exist.
  2. DROP TABLE inside a transaction is rolled back: table still exists with data.
  3. CREATE INDEX inside a transaction is rolled back: index does not exist.
  4. INSERT + CREATE TABLE interleaved, then ROLLBACK: both undone.
  5. DDL outside a transaction still works (auto-committed).
  6. ALTER TABLE DROP COLUMN is transactional: column reappears after ROLLBACK.
  7. DROP COLUMN + DML interleaved, then ROLLBACK: all undone.
  8. DROP COLUMN + COMMIT: column stays dropped, new rows work.
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

        # ---- 6. ALTER TABLE DROP COLUMN + ROLLBACK → column reappears ----
        # DROP COLUMN is now metadata-only (PostgreSQL-style) and fully
        # transactional. The column should reappear after ROLLBACK with
        # all original data intact.
        run_minidb(args.bin, db, [
            "ALTER TABLE baz ADD COLUMN a INT;",
            "ALTER TABLE baz ADD COLUMN b INT;",
            "INSERT INTO baz VALUES (1, 10, 20);",
        ])
        out7 = run_minidb(args.bin, db, [
            "BEGIN;",
            "ALTER TABLE baz DROP COLUMN b;",
            "ROLLBACK;",
        ])
        if "rolled back" not in out7.lower():
            raise AssertionError(
                f"DROP COLUMN transaction did not rollback cleanly: {out7}")
        # After rollback, column b should still be visible.
        rows = minidb_query(args.bin, db, "SELECT b FROM baz;", seed)
        if rows != [("20",)]:
            raise AssertionError(
                f"DROP COLUMN was not rolled back — column b missing or wrong: {rows}")

        # ---- 7. DROP COLUMN + DML interleaved + ROLLBACK → all undone ----
        out8 = run_minidb(args.bin, db, [
            "BEGIN;",
            "INSERT INTO baz VALUES (2, 30, 40);",
            "ALTER TABLE baz DROP COLUMN a;",
            "INSERT INTO baz VALUES (3, 50);",    # only z and b after drop
            "ROLLBACK;",
        ])
        # All changes should be rolled back: only the original row remains.
        rows = minidb_query(args.bin, db, "SELECT z, a, b FROM baz ORDER BY z;", seed)
        if rows != [("1", "10", "20")]:
            raise AssertionError(
                f"Interleaved DML+DROP COLUMN not fully rolled back: {rows}")

        # ---- 8. DROP COLUMN + COMMIT → column stays dropped, new rows work
        out9 = run_minidb(args.bin, db, [
            "BEGIN;",
            "ALTER TABLE baz DROP COLUMN b;",
            "INSERT INTO baz VALUES (4, 60);",    # only z and a after drop
            "COMMIT;",
        ])
        rows = minidb_query(args.bin, db, "SELECT z, a FROM baz ORDER BY z;", seed)
        if ("4", "60") not in rows:
            raise AssertionError(
                f"DROP COLUMN + COMMIT did not persist: {rows}")
        # Column b should no longer be accessible.
        b_check = run_minidb(args.bin, db, ["SELECT b FROM baz;"])
        if "Error" not in b_check and "not found" not in b_check.lower():
            raise AssertionError(
                f"Dropped column b still accessible after COMMIT: {b_check}")

        print(f"ddl_implicit_commit PASS seed={seed} "
              f"(all DDL transactional, including metadata-only DROP COLUMN)")
        return 0
    except Exception as exc:
        print(f"ddl_implicit_commit FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
