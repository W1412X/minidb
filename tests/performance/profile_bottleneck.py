#!/usr/bin/env python3
"""Profile MiniDB bottlenecks by toggling WAL fsync, GC, and checkpoint."""

from __future__ import annotations
import argparse
import os
import subprocess
import sys
import tempfile
import shutil
import time


def run_bench(bin_path: str, config: dict, label: str, stmts: list[str]) -> tuple[float, str]:
    db_dir = tempfile.mkdtemp(prefix="minidb-prof.")
    conf_lines = [f"{k}={v}" for k, v in config.items()]
    with open(os.path.join(db_dir, "minidb.conf"), "w") as f:
        f.write("\n".join(conf_lines) + "\n")

    if stmts[-1].strip().lower() != "exit":
        stmts = stmts + ["exit"]

    start = time.monotonic()
    proc = subprocess.run(
        [bin_path, "--dir", db_dir],
        input="\n".join(stmts) + "\n",
        text=True, capture_output=True, timeout=120,
    )
    elapsed = time.monotonic() - start
    out = proc.stdout + proc.stderr
    shutil.rmtree(db_dir, ignore_errors=True)
    return elapsed, out


def make_insert_stmts(n: int = 2000, batch_size: int = 40) -> list[str]:
    stmts = ["CREATE TABLE t (id INT PRIMARY KEY, val INT);"]
    batch = []
    for i in range(1, n + 1):
        batch.append(f"({i},{i*10})")
        if len(batch) >= batch_size:
            stmts.append(f"INSERT INTO t VALUES {','.join(batch)};")
            batch = []
    if batch:
        stmts.append(f"INSERT INTO t VALUES {','.join(batch)};")
    stmts.append("SELECT COUNT(*) FROM t;")
    return stmts


def make_update_stmts(n: int = 200) -> list[str]:
    """Requires table t already exists with 2000 rows."""
    stmts = []
    for i in range(1, n + 1):
        stmts.append(f"UPDATE t SET val = {i * 100} WHERE id = {i};")
    return stmts


def make_txn_insert_stmts(n: int = 2000, batch_size: int = 100) -> list[str]:
    stmts = ["CREATE TABLE t (id INT PRIMARY KEY, val INT);"]
    row = 0
    for _ in range(n // batch_size):
        stmts.append("BEGIN;")
        for _ in range(batch_size):
            row += 1
            stmts.append(f"INSERT INTO t VALUES ({row},{row*10});")
        stmts.append("COMMIT;")
    stmts.append("SELECT COUNT(*) FROM t;")
    return stmts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bin", nargs="?", default="./build/minidb")
    args = parser.parse_args()

    N_INSERT = 2000
    N_UPDATE = 200

    configs = {
        "baseline (fsync+gc)": {
            "wal_fsync": "on",
            "gc_enabled": "on",
            "gc_ops_threshold": "1",
        },
        "gc_threshold=50": {
            "wal_fsync": "on",
            "gc_enabled": "on",
            "gc_ops_threshold": "50",
        },
        "gc_disabled": {
            "wal_fsync": "on",
            "gc_enabled": "off",
        },
        "fsync_off + gc_on": {
            "wal_fsync": "off",
            "gc_enabled": "on",
            "gc_ops_threshold": "1",
        },
        "fsync_off + gc_off": {
            "wal_fsync": "off",
            "gc_enabled": "off",
        },
        "group_commit + gc_off": {
            "wal_fsync": "on",
            "wal_group_commit": "on",
            "wal_group_commit_delay": "1",
            "gc_enabled": "off",
        },
    }

    print("=" * 72)
    print(f"INSERT benchmark: {N_INSERT} rows in batches of 40 (autocommit)")
    print("-" * 72)
    print(f"  {'Config':<28s} {'Time':>8s} {'rows/s':>10s}")
    print("-" * 72)
    for label, cfg in configs.items():
        stmts = make_insert_stmts(N_INSERT)
        elapsed, out = run_bench(args.bin, cfg, label, stmts)
        err = "ERROR" if "Error" in out and "already exists" not in out.lower() else ""
        rate = N_INSERT / elapsed if not err else 0
        print(f"  {label:<28s} {elapsed:>7.2f}s {rate:>9.0f} r/s {err}")

    print()
    print("=" * 72)
    print(f"UPDATE benchmark: {N_UPDATE} single-row UPDATEs (autocommit)")
    print("-" * 72)
    print(f"  {'Config':<28s} {'Time':>8s} {'ops/s':>10s}")
    print("-" * 72)
    for label, cfg in configs.items():
        # Setup: create + insert, then update
        setup_stmts = make_insert_stmts(N_INSERT)
        update_stmts = make_update_stmts(N_UPDATE)
        all_stmts = setup_stmts + update_stmts
        # We only want to measure the update portion, but subprocess runs all at once.
        # Run setup separately, then measure updates.
        db_dir = tempfile.mkdtemp(prefix="minidb-prof-upd.")
        conf_lines = [f"{k}={v}" for k, v in cfg.items()]
        with open(os.path.join(db_dir, "minidb.conf"), "w") as f:
            f.write("\n".join(conf_lines) + "\n")
        # Setup
        subprocess.run(
            [args.bin, "--dir", db_dir],
            input="\n".join(setup_stmts + ["exit"]) + "\n",
            text=True, capture_output=True, timeout=120,
        )
        # Measure updates
        start = time.monotonic()
        proc = subprocess.run(
            [args.bin, "--dir", db_dir],
            input="\n".join(update_stmts + ["exit"]) + "\n",
            text=True, capture_output=True, timeout=120,
        )
        elapsed = time.monotonic() - start
        out = proc.stdout + proc.stderr
        shutil.rmtree(db_dir, ignore_errors=True)
        err = "ERROR" if "Error" in out and "timeout" not in out.lower() else ""
        rate = N_UPDATE / elapsed if not err else 0
        print(f"  {label:<28s} {elapsed:>7.2f}s {rate:>9.1f} o/s {err}")

    print()
    print("=" * 72)
    print(f"TXN batch INSERT: {N_INSERT} rows in txns of 100 (explicit BEGIN/COMMIT)")
    print("-" * 72)
    print(f"  {'Config':<28s} {'Time':>8s} {'rows/s':>10s}")
    print("-" * 72)
    for label, cfg in [("baseline (fsync+gc)", configs["baseline (fsync+gc)"]),
                       ("fsync_off + gc_off", configs["fsync_off + gc_off"])]:
        stmts = make_txn_insert_stmts(N_INSERT, 100)
        elapsed, out = run_bench(args.bin, cfg, label, stmts)
        rate = N_INSERT / elapsed
        print(f"  {label:<28s} {elapsed:>7.2f}s {rate:>9.0f} r/s")

    print("=" * 72)


if __name__ == "__main__":
    main()
