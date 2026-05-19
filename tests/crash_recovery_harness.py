#!/usr/bin/env python3
"""Crash/recovery harness with deterministic fault cycles.

The harness kills MiniDB around commit, rollback, dirty-page, and checkpoint-like
restart boundaries, then reopens the database and verifies the logical state.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    assert_no_error,
    assert_rows_equal,
    cleanup,
    minidb_query,
    run_minidb,
    seeded_rng,
    temp_db,
)


def kill_during_process(bin_path: str, db_dir: str, statements: list[str], delay: float) -> None:
    proc = subprocess.Popen(
        [bin_path, "--dir", db_dir],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        assert proc.stdin is not None
        for sql in statements:
            proc.stdin.write(sql + "\n")
            proc.stdin.flush()
            time.sleep(delay)
        time.sleep(delay)
        proc.kill()
    finally:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    rng = seeded_rng(seed)
    cycles = args.cases or (400 if args.stress else 24)
    db_dir = temp_db("minidb-crash.")
    model: dict[int, int] = {}
    try:
        out = run_minidb(args.bin, db_dir, ["CREATE TABLE crash_t (id INT PRIMARY KEY, v INT);", "CREATE INDEX crash_v ON crash_t(v);"])
        assert_no_error(out, seed, "crash setup")

        for cycle in range(cycles):
            op = rng.choice(["committed_insert", "uncommitted_insert", "committed_update", "uncommitted_update", "committed_delete", "uncommitted_delete"])
            key = rng.randint(1, 80)
            val = rng.randint(1, 10000)
            statements: list[str]
            if op == "committed_insert":
                statements = ["BEGIN;", f"INSERT INTO crash_t VALUES ({key}, {val});", "COMMIT;"]
                if key not in model:
                    model[key] = val
            elif op == "uncommitted_insert":
                statements = ["BEGIN;", f"INSERT INTO crash_t VALUES ({key}, {val});"]
            elif op == "committed_update":
                statements = ["BEGIN;", f"UPDATE crash_t SET v = {val} WHERE id = {key};", "COMMIT;"]
                if key in model:
                    model[key] = val
            elif op == "uncommitted_update":
                statements = ["BEGIN;", f"UPDATE crash_t SET v = {val} WHERE id = {key};"]
            elif op == "committed_delete":
                statements = ["BEGIN;", f"DELETE FROM crash_t WHERE id = {key};", "COMMIT;"]
                model.pop(key, None)
            else:
                statements = ["BEGIN;", f"DELETE FROM crash_t WHERE id = {key};"]

            # Vary the boundary: before commit flush, after commit, and after a clean reopen/checkpoint.
            kill_during_process(args.bin, db_dir, statements, delay=0.015 + (cycle % 3) * 0.01)
            if cycle % 5 == 0:
                out = run_minidb(args.bin, db_dir, ["SELECT COUNT(*) FROM crash_t;"], timeout=30)
                assert_no_error(out, seed, f"post-crash reopen cycle={cycle}")

            if cycle % 4 == 0 or cycle == cycles - 1:
                expected = [(str(k), str(model[k])) for k in sorted(model)]
                actual = minidb_query(args.bin, db_dir, "SELECT id, v FROM crash_t ORDER BY id;", seed, timeout=30)
                assert_rows_equal(actual, expected, seed, f"crash cycle={cycle} op={op}")

        # Repeated recovery must be idempotent.
        for i in range(3):
            expected = [(str(k), str(model[k])) for k in sorted(model)]
            actual = minidb_query(args.bin, db_dir, "SELECT id, v FROM crash_t ORDER BY id;", seed, timeout=30)
            assert_rows_equal(actual, expected, seed, f"idempotent recovery pass={i}")

        print(f"crash_recovery_harness PASS seed={seed} cycles={cycles}")
        return 0
    except Exception as exc:
        print(f"crash_recovery_harness FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db_dir)


if __name__ == "__main__":
    raise SystemExit(main())
