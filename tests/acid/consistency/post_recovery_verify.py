#!/usr/bin/env python3
"""Startup consistency check test for ACID item C4.

Exercises both branches of the consistency_check_on_startup config:

  - Positive: a healthy database (heap and indexes agree) opens
    successfully when the flag is on.
  - Negative: a database where some heap rows have no matching index
    entry refuses to open. The inconsistency is synthesized with the
    MINIDB_FAULT=index_insert_silent injection point so the test does
    not have to corrupt files on disk and risk hanging the B+ tree on
    load.

Together these prove the wiring: the check is invoked at startup, it
actually compares heap rows against index entries, and it fails the
process loudly (non-zero exit, descriptive stderr) rather than silently
serving a half-corrupt database.
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
    temp_db,
)


def write_config(db_dir: str, check_on_startup: bool) -> str:
    path = os.path.join(db_dir, "minidb.conf")
    with open(path, "w") as f:
        if check_on_startup:
            f.write("consistency_check_on_startup = on\n")
        else:
            f.write("consistency_check_on_startup = off\n")
    return path


def run_minidb(bin_path: str, db_dir: str, statements: list[str],
               *, fault: str | None = None, config: str | None = None,
               timeout: int = 15) -> subprocess.CompletedProcess[str]:
    if not statements or statements[-1].strip().lower() != "exit":
        statements = list(statements) + ["exit"]
    env = os.environ.copy()
    if fault is not None:
        env["MINIDB_FAULT"] = fault
    cmd = [bin_path, "--dir", db_dir]
    if config:
        cmd += ["--config", config]
    return subprocess.run(
        cmd,
        input="\n".join(statements) + "\n",
        text=True,
        capture_output=True,
        timeout=timeout,
        env=env,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed

    # ---- positive: healthy DB opens cleanly with the check on -----
    db = temp_db("minidb-c4-pos.")
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX t_v ON t(v);",
            "INSERT INTO t VALUES (1, 10);",
            "INSERT INTO t VALUES (2, 20);",
        ])
        if "Error" in (setup.stdout + setup.stderr) or setup.returncode != 0:
            raise AssertionError(
                f"setup failed: rc={setup.returncode}\n{setup.stdout}{setup.stderr}")

        cfg = write_config(db, check_on_startup=True)
        reopen = run_minidb(args.bin, db, ["SELECT id FROM t ORDER BY id;"],
                            config=cfg)
        if reopen.returncode != 0 or "1" not in reopen.stdout or "2" not in reopen.stdout:
            raise AssertionError(
                f"healthy DB rejected by consistency check (positive case)\n"
                f"rc={reopen.returncode}\nstdout={reopen.stdout}\n"
                f"stderr={reopen.stderr}")
    finally:
        cleanup(db)

    # ---- negative: a heap row with no index entry must be detected -----
    db = temp_db("minidb-c4-neg.")
    try:
        # Synthesize the inconsistency with the test-only fault. Heap rows
        # land normally, index entries are silently skipped, no error is
        # raised. This mimics what the pre-A2 code did on partial failures.
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
            "CREATE INDEX t_v ON t(v);",
            "INSERT INTO t VALUES (1, 10);",
            "INSERT INTO t VALUES (2, 20);",
        ], fault="index_insert_silent")
        if setup.returncode != 0:
            raise AssertionError(
                f"fault-injected setup failed: rc={setup.returncode}\n"
                f"{setup.stdout}{setup.stderr}")

        # Without the check, the database opens but is silently corrupt.
        cfg_off = write_config(db, check_on_startup=False)
        plain = run_minidb(args.bin, db,
                           ["SELECT COUNT(*) FROM t;"], config=cfg_off)
        if plain.returncode != 0:
            raise AssertionError(
                f"DB should open with check off (negative baseline): "
                f"rc={plain.returncode}\n{plain.stderr}")

        # With the check on, startup must fail loudly.
        cfg_on = write_config(db, check_on_startup=True)
        guarded = run_minidb(args.bin, db, ["SELECT 1;"], config=cfg_on)
        if guarded.returncode == 0:
            raise AssertionError(
                f"corrupted DB opened with check on (seed={seed})\n"
                f"stdout={guarded.stdout}\nstderr={guarded.stderr}")
        if "consistency check failed" not in guarded.stderr:
            raise AssertionError(
                f"expected 'consistency check failed' on stderr, got:\n"
                f"{guarded.stderr}")
        if "heap tuple missing from index" not in guarded.stderr:
            raise AssertionError(
                f"expected the missing-index diagnostic, got:\n{guarded.stderr}")
    finally:
        cleanup(db)

    print(f"post_recovery_verify PASS seed={seed}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"post_recovery_verify FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
