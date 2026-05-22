#!/usr/bin/env python3
"""Regression test for the UPDATE Halloween problem.

Prior to the fix, the HOT-eligible branch of UpdateExecutor streamed
from `child_->next()` and immediately mutated the heap, so a newly
written same-page HOT version was visible to subsequent child_->next()
calls (it satisfies "own write" snapshot visibility). The WHERE
predicate then matched the new version too, the executor updated it
again, and the loop only stopped when the page filled up — inflating
both affected_rows and the column value to ~100x the intended.

This test guards the fix in update.cpp that always materialises the
target RID list BEFORE applying any updates, decoupling scan from
mutation. Three shapes are exercised:

  1. Bare table (SeqScan + Filter path).
  2. Table with PRIMARY KEY (IndexScan path).
  3. Table with secondary index on a different column (HOT path with
     index).
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    minidb_query,
    run_minidb,
    temp_db,
)


def assert_affected(output: str, expected: int, ctx: str, seed: int) -> None:
    """Extract `affected_rows\\nN` from the raw REPL output and compare."""
    lines = [ln.strip() for ln in output.splitlines() if ln.strip()]
    seen = 0
    for i, ln in enumerate(lines):
        if ln == "affected_rows" and i + 1 < len(lines):
            seen += 1
            try:
                got = int(lines[i + 1])
            except ValueError:
                continue
            if seen == 1:
                continue  # the INSERT affected_rows
            if got != expected:
                raise AssertionError(
                    f"{ctx}: expected affected_rows={expected}, got {got} (seed={seed})"
                )
            return
    raise AssertionError(f"{ctx}: could not find a second affected_rows line (seed={seed})\n{output}")


def run_case(bin_path: str, ddl: str, seed: int, label: str) -> None:
    db = temp_db(f"minidb-halloween-{label}.")
    try:
        out = run_minidb(bin_path, db, [
            ddl,
            "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
            "UPDATE t SET v = v + 10 WHERE id = 1;",
        ])
        if "Error" in out:
            raise AssertionError(f"{label}: unexpected error: {out}")
        assert_affected(out, 1, label, seed)

        rows = minidb_query(bin_path, db, "SELECT id, v FROM t ORDER BY id;", seed)
        expected = [("1", "11"), ("2", "2"), ("3", "3")]
        if rows != expected:
            raise AssertionError(
                f"{label}: rows mismatch (seed={seed})\nactual={rows}\nexpected={expected}"
            )
    finally:
        cleanup(db)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    try:
        # SeqScan + Filter — the path the bug originally inflated to 149.
        run_case(args.bin, "CREATE TABLE t (id INT, v INT);", seed, "seqscan")
        # IndexScan via primary key — should already be safe but worth pinning.
        run_case(args.bin, "CREATE TABLE t (id INT PRIMARY KEY, v INT);", seed, "indexscan")
        # HOT path with a secondary index on `v` that the UPDATE does not touch.
        db = temp_db("minidb-halloween-secidx.")
        try:
            out = run_minidb(args.bin, db, [
                "CREATE TABLE t (id INT, v INT, pad TEXT);",
                "CREATE INDEX t_pad_idx ON t(pad);",
                "INSERT INTO t VALUES (1, 1, 'a'), (2, 2, 'b'), (3, 3, 'c');",
                "UPDATE t SET v = v + 10 WHERE id = 1;",
            ])
            if "Error" in out:
                raise AssertionError(f"secidx: unexpected error: {out}")
            assert_affected(out, 1, "secidx", seed)
            rows = minidb_query(args.bin, db,
                                "SELECT id, v FROM t ORDER BY id;", seed)
            if rows != [("1", "11"), ("2", "2"), ("3", "3")]:
                raise AssertionError(
                    f"secidx: rows mismatch (seed={seed}) actual={rows}"
                )
        finally:
            cleanup(db)

        print(f"update_halloween PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"update_halloween FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
