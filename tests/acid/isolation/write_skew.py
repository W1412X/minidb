#!/usr/bin/env python3
"""Documents MiniDB's snapshot-isolation write-skew anomaly (ACID I3).

Snapshot isolation (SI) detects lost updates (two transactions writing
the same row) but does NOT detect write skew (two transactions writing
DIFFERENT rows whose updates jointly violate a predicate).

This test reproduces Berenson et al.'s classic doctors-on-call case:

  rule: at least one doctor must remain on call.
  start: alice = on_call, bob = on_call.
  tx A reads count(on_call)=2, sets alice = off_call.
  tx B reads count(on_call)=2, sets bob = off_call.
  Both commit. End state: nobody on call → rule violated.

A serializable database would force one of A or B to abort. MiniDB,
being SI, lets both commit. This test asserts that this anomaly is
still present — it is intentional behaviour given the documented
isolation level (docs/CONCURRENCY_CONTROL.md). If/when MiniDB grows
SSI, this test should be updated to expect the abort instead, and the
README/docs flipped accordingly.
"""

from __future__ import annotations

import argparse
import os
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    TcpMiniDB,
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
    db = temp_db("minidb-write-skew.")
    port = 18000 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE doctors (id INT PRIMARY KEY, on_call INT);",
            "INSERT INTO doctors VALUES (1, 1);",  # alice on call
            "INSERT INTO doctors VALUES (2, 1);",  # bob on call
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()
        replies: dict[int, str] = {}
        ready = threading.Barrier(2)

        # Tx A updates alice (id=1), tx B updates bob (id=2). Different
        # rows → no row-level conflict under SI. Both will commit.
        def worker(tid: int) -> None:
            target = 1 if tid == 0 else 2
            ready.wait()
            replies[tid] = server.execute([
                "BEGIN;",
                # Each reader sees count(on_call) = 2 in its snapshot, so
                # decides it is safe to set its target off-call.
                f"UPDATE doctors SET on_call = 0 WHERE id = {target};",
                "COMMIT;",
            ], read_timeout=2.0)

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)

        failures = [r for r in replies.values() if "Error" in r]
        if failures:
            raise AssertionError(
                "MiniDB unexpectedly rejected the write-skew workload "
                "(seed={}). Under SI, both updates should commit because "
                "they touch different rows. If MiniDB has been upgraded to "
                "SSI, update this test and docs/CONCURRENCY_CONTROL.md.\n"
                "replies={}".format(seed, replies)
            )

        rows = minidb_query(args.bin, db,
                            "SELECT id, on_call FROM doctors ORDER BY id;",
                            seed)
        on_call = [r for r in rows if r[1] == "1"]
        if on_call:
            raise AssertionError(
                "expected SI write-skew anomaly: nobody on call after both "
                "commits. Got rows={} seed={}".format(rows, seed))

        print(f"write_skew PASS seed={seed} (SI anomaly observed as documented)")
        return 0
    except Exception as exc:
        print(f"write_skew FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
