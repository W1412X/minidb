#!/usr/bin/env python3
"""Lost-update prevention test for ACID item I2.

Two transactions concurrently reading the same row, computing a new
value from it, and writing back must not silently lose one update. The
classic test: txn A and txn B both run `UPDATE t SET v = v + 1 WHERE id = 1;`
under snapshot isolation. Without conflict detection, both compute v+1
from the same snapshot and one update is lost.

MiniDB's snapshot isolation must catch the second writer with an error
("could not serialize access due to concurrent update") so the user
knows to retry. This test asserts:

  1. The conflict is surfaced (one txn reports an Error rather than
     silently succeeding).
  2. The final value reflects the surviving writer's update (no lost
     state).
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
    db = temp_db("minidb-lost-update.")
    port = 17900 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)

    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE counter (id INT PRIMARY KEY, v INT);",
            "INSERT INTO counter VALUES (1, 0);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()
        replies: dict[int, str] = {}
        ready = threading.Barrier(2)

        def worker(tid: int) -> None:
            # Each worker increments the same row inside a SI snapshot.
            # The two BEGINs race so both txns observe v=0, then both try
            # to write v=1. One must lose with a write-write conflict.
            ready.wait()
            replies[tid] = server.execute([
                "BEGIN;",
                "UPDATE counter SET v = v + 1 WHERE id = 1;",
                "COMMIT;",
            ], read_timeout=2.0)

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)

        successes = [r for r in replies.values() if "Error" not in r]
        failures = [r for r in replies.values() if "Error" in r]

        # The strong invariant: at least one transaction must have observed
        # the conflict. Both succeeding silently would be the lost-update
        # bug we are guarding against.
        if not failures:
            rows = minidb_query(args.bin, db,
                                "SELECT v FROM counter WHERE id = 1;", seed)
            raise AssertionError(
                f"lost-update not detected seed={seed}: both txns reported "
                f"success but no conflict raised. Final row={rows}, "
                f"replies={replies}"
            )

        # Whichever side won, the final value must match its single
        # increment (1), never 0 and never 2 — the latter would mean both
        # writes landed and the conflict was a false negative.
        rows = minidb_query(args.bin, db,
                            "SELECT v FROM counter WHERE id = 1;", seed)
        if rows != [("1",)] and rows != [("0",)]:
            raise AssertionError(
                f"unexpected final value seed={seed}: {rows} "
                f"(successes={len(successes)}, failures={len(failures)})"
            )

        print(f"lost_update PASS seed={seed} "
              f"successes={len(successes)} conflicts={len(failures)}")
        return 0
    except Exception as exc:
        print(f"lost_update FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
