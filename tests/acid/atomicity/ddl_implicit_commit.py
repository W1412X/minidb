#!/usr/bin/env python3
"""DDL implicitly commits the surrounding user transaction (ACID A/D).

MiniDB does not have full transactional DDL — `save_catalog()` writes
the catalog atomically to disk the moment a CREATE / DROP / ALTER runs.
Before this change a sequence like

    BEGIN;
    INSERT INTO t VALUES (...);
    CREATE TABLE foo (...);
    ROLLBACK;

would silently keep `foo` (the catalog write happened immediately) but
roll back the INSERT. That mismatch is the classic DDL footgun.

The fix matches MySQL / SQL-standard "implicit commit" semantics: any
DDL inside an explicit transaction first COMMITs the open transaction,
then runs the DDL as its own auto-committed statement.

The test pins:
  1. INSERTs before the DDL are committed (the implicit commit fires).
  2. The DDL itself persists past a follow-up ROLLBACK.
  3. ROLLBACK after the implicit commit reports "no active transaction"
     because BEGIN..DDL already closed the txn.
  4. Subsequent DDL outside a txn still works.
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
    db = temp_db("minidb-ddl-implicit.")
    try:
        # Set up an existing table.
        run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT);",
            "INSERT INTO t VALUES (1);",
        ])

        # ---- BEGIN; INSERT; CREATE TABLE; ROLLBACK ----------------
        # Expectation: the INSERT before the DDL is committed (implicit
        # commit fired), CREATE TABLE persists, ROLLBACK is a no-op.
        out = run_minidb(args.bin, db, [
            "BEGIN;",
            "INSERT INTO t VALUES (2);",
            "CREATE TABLE foo (x INT);",   # implicit commit here
            "ROLLBACK;",                   # no active txn -> error msg
        ])
        if "no active transaction" not in out:
            raise AssertionError(
                f"expected ROLLBACK-after-implicit-commit to report no active txn, got: {out}")

        # The INSERT committed via implicit commit.
        rows = minidb_query(args.bin, db, "SELECT id FROM t ORDER BY id;", seed)
        if rows != [("1",), ("2",)]:
            raise AssertionError(f"pre-DDL INSERT was lost: {rows}")

        # The CREATE TABLE persisted across the ROLLBACK.
        rows = minidb_query(args.bin, db, "SELECT * FROM foo;", seed)
        # Empty table, just header — assert query did NOT error.
        if "Error" in str(rows):
            raise AssertionError(f"DDL was rolled back: {rows}")

        # ---- DDL outside a txn still works ------------------------
        out2 = run_minidb(args.bin, db, ["CREATE TABLE bar (y INT);"])
        if "Error" in out2:
            raise AssertionError(f"plain DDL rejected: {out2}")

        # ---- BEGIN; DDL; verify txn closed ------------------------
        out3 = run_minidb(args.bin, db, [
            "BEGIN;",
            "CREATE TABLE baz (z INT);",
            "COMMIT;",   # nothing to commit -> no active txn
        ])
        if "no active transaction" not in out3:
            raise AssertionError(
                f"BEGIN; DDL; COMMIT should leave no active txn, got: {out3}")

        print(f"ddl_implicit_commit PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"ddl_implicit_commit FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
