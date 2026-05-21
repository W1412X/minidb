#!/usr/bin/env python3
"""Index/heap atomicity test for ACID item A2.

Before A2: Database::insert_index_entries returned void and ignored
BPlusTree::insert's bool. A failure in an index insert left the heap
tuple in place with no index entry pointing at it, breaking
Consistency invariants (SELECT via index would miss the row, while
SeqScan would see it; UNIQUE constraints could be bypassed).

After A2:
  - insert_index_entries returns bool;
  - InsertExecutor and UpdateExecutor record the heap-insert undo
    BEFORE attempting index inserts;
  - on false they surface Error and return, and the active
    transaction's rollback path undoes the heap row plus any partial
    index entries via delete_index_entries (idempotent).

This test deterministically triggers an index-insert failure with the
MINIDB_FAULT=index_insert_fail env var, then asserts:

  1. The failing INSERT does NOT leave a row visible to either SeqScan
     or IndexScan.
  2. A subsequent INSERT with the same key succeeds (proving the
     in-memory unique-key reservation was released too).
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
    temp_db,
)


def run_with_fault(bin_path: str, db_dir: str, fault: str | None,
                   statements: list[str], timeout: int = 30) -> str:
    if not statements or statements[-1].strip().lower() != "exit":
        statements = list(statements) + ["exit"]
    env = os.environ.copy()
    if fault is not None:
        env["MINIDB_FAULT"] = fault
    proc = subprocess.run(
        [bin_path, "--dir", db_dir],
        input="\n".join(statements) + "\n",
        text=True,
        capture_output=True,
        timeout=timeout,
        env=env,
    )
    return proc.stdout + proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-a2-atomic.")
    try:
        setup = run_with_fault(args.bin, db, None, [
            "CREATE TABLE atom (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX atom_v_idx ON atom(v);",
            "INSERT INTO atom VALUES (1, 100);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")
        rows = minidb_query(args.bin, db, "SELECT id, v FROM atom ORDER BY id;", seed)
        if rows != [("1", "100")]:
            raise AssertionError(f"initial state wrong: {rows}")

        # Run an INSERT under MINIDB_FAULT=index_insert_fail. The heap
        # insert will succeed; the very next index insert must fail and
        # InsertExecutor must surface 'Error: index insert failed'.
        faulted = run_with_fault(args.bin, db, "index_insert_fail", [
            "INSERT INTO atom VALUES (2, 200);",
        ])
        if "Error" not in faulted:
            raise AssertionError(
                f"expected an error on faulted INSERT, got: {faulted}")

        # The row must not have leaked through either access path.
        rows_after_fault = minidb_query(args.bin, db,
                                        "SELECT id, v FROM atom ORDER BY id;",
                                        seed)
        if any(r[0] == "2" for r in rows_after_fault):
            raise AssertionError(
                f"row id=2 visible via SeqScan after faulted INSERT: "
                f"{rows_after_fault}")
        idx_rows = minidb_query(args.bin, db,
                                "SELECT id, v FROM atom WHERE v = 200;",
                                seed)
        if idx_rows:
            raise AssertionError(
                f"row id=2 visible via index after faulted INSERT: {idx_rows}")

        # The unique-key slot must be free again: a retry without the
        # fault must succeed.
        retry = run_with_fault(args.bin, db, None, [
            "INSERT INTO atom VALUES (2, 200);",
        ])
        if "Error" in retry:
            raise AssertionError(
                f"retry INSERT after faulted attempt failed: {retry}")
        final = minidb_query(args.bin, db, "SELECT id, v FROM atom ORDER BY id;",
                             seed)
        if final != [("1", "100"), ("2", "200")]:
            raise AssertionError(f"final state wrong: {final}")
        idx_final = minidb_query(args.bin, db,
                                 "SELECT id FROM atom WHERE v = 200;", seed)
        if idx_final != [("2",)]:
            raise AssertionError(f"index final state wrong: {idx_final}")

        print(f"index_heap_atomic PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"index_heap_atomic FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
