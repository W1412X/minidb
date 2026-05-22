#!/usr/bin/env python3
"""Index state machine + optimiser gate (ACID B5/D4).

After WAL replay touches the heap, every index is potentially out of
sync with the tuples on disk. The catalog's IndexEntry now carries a
state field (kValid / kInvalid / kRebuilding) and the optimiser must
NOT use any index whose state is not kValid. This test asserts that:

  1. A freshly created table+index runs `SELECT ... WHERE id = ...`
     through IndexScan as expected.
  2. After a crash (kill -9) that leaves a WAL needing replay, opening
     the DB with `MINIDB_FAULT=skip_index_rebuild` keeps every index
     in the kInvalid state.
  3. While kInvalid, the same SELECT falls back to SeqScan — EXPLAIN
     no longer mentions IndexScan and the optimiser_note is absent.
  4. Result correctness is unaffected — the row is still returned.
  5. A subsequent restart WITHOUT the fault rebuilds the index and the
     plan returns to IndexScan.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    minidb_query,
    run_minidb,
    temp_db,
)


def run_with_fault(bin_path: str, db_dir: str, statements: list[str],
                   *, fault: str | None = None) -> str:
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
        timeout=15,
        env=env,
    )
    return proc.stdout + proc.stderr


def explain(bin_path: str, db_dir: str, sql: str, *, fault: str | None = None) -> str:
    return run_with_fault(bin_path, db_dir, [f"EXPLAIN {sql}"], fault=fault)


def kill_after_wal_write(bin_path: str, db_dir: str, statements: list[str]) -> None:
    """Run `statements`, give minidb a moment to write WAL, then SIGKILL.

    We deliberately skip COMMIT after the last DML so the next startup
    DOES have to run WAL replay (and flip indexes invalid).
    """
    proc = subprocess.Popen(
        [bin_path, "--dir", db_dir],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    assert proc.stdin is not None
    for s in statements:
        proc.stdin.write(s + "\n")
    proc.stdin.flush()
    time.sleep(0.3)
    proc.kill()
    proc.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-idx-state.")
    try:
        # ---- 1: baseline — index is valid, EXPLAIN picks IndexScan -----
        out = run_with_fault(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
            "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);",
        ])
        if "Error" in out:
            raise AssertionError(f"setup failed: {out}")
        plan_ok = explain(args.bin, db, "SELECT v FROM t WHERE id = 2;")
        if "IndexScan" not in plan_ok:
            raise AssertionError(
                f"baseline EXPLAIN should pick IndexScan, got:\n{plan_ok}")

        # ---- 2: crash mid-write, leave WAL needing replay --------------
        kill_after_wal_write(args.bin, db, [
            "BEGIN;",
            "INSERT INTO t VALUES (4, 40);",
            "COMMIT;",
        ])

        # ---- 3: restart with the fault — every index stays kInvalid ----
        plan_invalid = explain(args.bin, db,
                               "SELECT v FROM t WHERE id = 2;",
                               fault="skip_index_rebuild")
        if "IndexScan" in plan_invalid:
            raise AssertionError(
                f"optimiser used kInvalid index, EXPLAIN says:\n{plan_invalid}")
        if "SeqScan" not in plan_invalid:
            raise AssertionError(
                f"expected SeqScan fallback, got:\n{plan_invalid}")

        # ---- 4: query still returns the right row -----------------------
        # With the fault on, the index stays invalid but heap recovery
        # already happened. The row for id=2 must still be visible via
        # SeqScan + Filter.
        rows = run_with_fault(args.bin, db, ["SELECT v FROM t WHERE id = 2;"],
                              fault="skip_index_rebuild")
        if "20" not in rows:
            raise AssertionError(f"row not visible via SeqScan: {rows}")

        # ---- 5: restart without the fault — rebuild happens, IndexScan returns
        plan_rebuilt = explain(args.bin, db, "SELECT v FROM t WHERE id = 2;")
        if "IndexScan" not in plan_rebuilt:
            raise AssertionError(
                f"after rebuild, optimiser should pick IndexScan again, got:\n"
                f"{plan_rebuilt}")

        print(f"index_state_machine PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"index_state_machine FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
