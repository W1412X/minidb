#!/usr/bin/env python3
"""Regression: LSN watermark must survive clean-shutdown WAL truncation.

Prior bug: clean shutdown truncated the WAL file but the next session's
WalManager re-initialised next_lsn_/durable_lsn_/last_written_lsn_ to 1
and 0. Pages on disk still carried LSNs from the prior session, so a
checkpoint after restart wrote its kCheckpoint record at an LSN
SMALLER than existing page LSNs. The D2 checkpoint barrier (which
holds the WAL latch through the page-flush callback) then deadlocked
on the very first dirty page whose page_lsn exceeded durable_lsn:
BufferPool::flush_frame_wal_first tried to re-acquire the WAL latch
via WalManager::flush_until.

The fix:
  - control file already records checkpoint_lsn on clean shutdown;
    load_control_file() now reads it and calls
    wal_->ensure_next_lsn_at_least() before recovery runs;
  - WalManager::recover() advances rather than resets the LSN counters,
    so the watermark survives even when the WAL file is empty.

The classic trigger was `ALTER TABLE t DROP COLUMN x` on a reopened
database: the column rewrite path mutates pages in place, then calls
checkpoint(), which deadlocked at the first dirty page. This test
runs that exact scenario plus a couple of close cousins.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    run_minidb,
    temp_db,
)


def run_after_restart(bin_path: str, db: str, sqls: list[str], *, timeout: int = 8) -> str:
    """Run sqls in a fresh process against an existing DB dir; bound by timeout."""
    if not sqls or sqls[-1].strip().lower() != "exit":
        sqls = list(sqls) + ["exit"]
    proc = subprocess.run(
        [bin_path, "--dir", db],
        input="\n".join(sqls) + "\n",
        capture_output=True, text=True, timeout=timeout,
    )
    return proc.stdout + proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed

    cases = [
        ("alter_drop_column_after_restart", [
            "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR);",
            "INSERT INTO t VALUES (1, 'alice');",
            "ALTER TABLE t ADD COLUMN age INT DEFAULT 25;",
        ], [
            "ALTER TABLE t DROP COLUMN age;",
            "SELECT id, name FROM t;",
        ]),
        ("checkpoint_after_restart", [
            "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
            "INSERT INTO t VALUES (1, 1), (2, 2);",
        ], [
            # Force a write that dirties a page, then exit. The destructor
            # runs checkpoint(); if next_lsn watermark is not restored, the
            # page_lsn > durable_lsn deadlock fires here.
            "UPDATE t SET v = 99 WHERE id = 1;",
        ]),
    ]

    try:
        for label, session1, session2 in cases:
            db = temp_db(f"minidb-lsn-{label}.")
            try:
                out1 = run_minidb(args.bin, db, session1)
                if "Error" in out1:
                    raise AssertionError(f"{label} setup failed: {out1}")
                try:
                    out2 = run_after_restart(args.bin, db, session2)
                except subprocess.TimeoutExpired:
                    raise AssertionError(
                        f"{label}: restart session hung (LSN watermark not restored) "
                        f"(seed={seed})"
                    )
                if "Error" in out2 and "constraint" not in out2 and "not found" not in out2:
                    raise AssertionError(f"{label} restart unexpected error: {out2}")
            finally:
                cleanup(db)

        print(f"lsn_watermark_restart PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"lsn_watermark_restart FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
