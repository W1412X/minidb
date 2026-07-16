#!/usr/bin/env python3
"""C2: WAL DML write failure must not mutate heap/index pages.

With MINIDB_FAULT=wal_dml_fail, insert/update/delete log records return LSN 0.
Executors must fail the statement closed so a subsequent clean restart (no
WAL redo for those rows) cannot observe unlogged durable data.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    minidb_query,
    run_minidb,
    temp_db,
)


def run_with_fault(bin_path: str, db_dir: str, sqls: list[str]) -> str:
    env = os.environ.copy()
    env["MINIDB_FAULT"] = "wal_dml_fail"
    proc = subprocess.run(
        [bin_path, "--dir", db_dir],
        input="\n".join(sqls) + "\nexit;\n",
        text=True,
        capture_output=True,
        env=env,
        timeout=30,
    )
    return (proc.stdout or "") + (proc.stderr or "")


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-wal-dml-fail.")
    try:
        out = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
            "INSERT INTO t VALUES (1, 10);",
        ])
        if "Error" in out:
            raise AssertionError(f"setup failed: {out}")

        # Under the fault, INSERT must fail and leave no new row.
        bad_ins = run_with_fault(args.bin, db, [
            "INSERT INTO t VALUES (2, 20);",
            "SELECT id FROM t ORDER BY id;",
        ])
        if "WAL write failed" not in bad_ins and "Error" not in bad_ins:
            raise AssertionError(f"expected WAL failure on insert: {bad_ins}")
        rows = minidb_query(args.bin, db, "SELECT id, v FROM t ORDER BY id;", seed)
        if rows != [("1", "10")]:
            raise AssertionError(f"unlogged insert persisted: {rows}")

        # UPDATE must fail closed.
        bad_upd = run_with_fault(args.bin, db, [
            "UPDATE t SET v = 99 WHERE id = 1;",
            "SELECT id, v FROM t WHERE id = 1;",
        ])
        if "WAL write failed" not in bad_upd and "Error" not in bad_upd:
            raise AssertionError(f"expected WAL failure on update: {bad_upd}")
        rows = minidb_query(args.bin, db, "SELECT id, v FROM t WHERE id = 1;", seed)
        if rows != [("1", "10")]:
            raise AssertionError(f"unlogged update persisted: {rows}")

        # DELETE must fail closed.
        bad_del = run_with_fault(args.bin, db, [
            "DELETE FROM t WHERE id = 1;",
            "SELECT COUNT(*) FROM t;",
        ])
        if "WAL write failed" not in bad_del and "Error" not in bad_del:
            raise AssertionError(f"expected WAL failure on delete: {bad_del}")
        rows = minidb_query(args.bin, db, "SELECT COUNT(*) FROM t;", seed)
        if rows != [("1",)]:
            raise AssertionError(f"unlogged delete persisted: {rows}")

        print(f"wal_dml_fail_closed PASS seed={seed}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"wal_dml_fail_closed FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    sys.exit(main())
