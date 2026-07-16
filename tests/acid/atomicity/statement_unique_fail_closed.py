#!/usr/bin/env python3
"""Multi-row INSERT/UPDATE must fail closed on UNIQUE conflicts.

A statement that applies earlier rows then hits a unique violation must not
silently skip the conflict and report success — the statement savepoint /
implicit txn must undo the partial work.
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
    db = temp_db("minidb-stmt-unique.")
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, u INT UNIQUE);",
            "INSERT INTO t VALUES (1, 10);",
            "INSERT INTO t VALUES (2, 20);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        # Multi-value INSERT: first row would succeed, second conflicts on u=10.
        out = run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (3, 30), (4, 10);",
        ])
        if "Error" not in out and "duplicate" not in out.lower():
            raise AssertionError(f"expected unique error on multi-insert: {out}")

        rows = minidb_query(args.bin, db, "SELECT id, u FROM t ORDER BY id;", seed)
        if rows != [("1", "10"), ("2", "20")]:
            raise AssertionError(f"partial multi-insert leaked: {rows}")

        # Multi-row UPDATE that would make two rows share u=20.
        out = run_minidb(args.bin, db, [
            "UPDATE t SET u = 20 WHERE id = 1;",
        ])
        if "Error" not in out and "duplicate" not in out.lower():
            raise AssertionError(f"expected unique error on update: {out}")

        rows = minidb_query(args.bin, db, "SELECT id, u FROM t ORDER BY id;", seed)
        if rows != [("1", "10"), ("2", "20")]:
            raise AssertionError(f"failed update mutated row: {rows}")

        print(f"statement_unique_fail_closed PASS seed={seed}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"statement_unique_fail_closed FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    sys.exit(main())
