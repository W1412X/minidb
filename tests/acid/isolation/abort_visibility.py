#!/usr/bin/env python3
"""C1: aborted inserts/deletes must never become visible to new snapshots.

Covers the end-to-end invariant that ROLLBACK leaves no dirty rows for a
subsequent autocommit reader. The narrow mid-rollback race is guarded by
tests/unit/abort_visibility_test.cpp; this script locks the SQL-level
contract under concurrent TCP sessions hammering INSERT+ROLLBACK.
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
    assert_no_error,
    cleanup,
    run_minidb,
    select_rows_from_output,
    temp_db,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    workers = 4 if not args.stress else 16
    rounds = args.cases or (20 if not args.stress else 100)
    db_dir = temp_db("minidb-abort-vis.")
    port = 18000 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db_dir, port)
    try:
        out = run_minidb(args.bin, db_dir, [
            "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
            "INSERT INTO t VALUES (0, 0);",
        ])
        assert_no_error(out, seed, "setup")

        server.start()
        errors: list[str] = []
        barrier = threading.Barrier(workers)

        def worker(tid: int) -> None:
            try:
                barrier.wait(timeout=10)
                for i in range(rounds):
                    key = 100000 + tid * 10000 + i
                    out = server.execute([
                        "BEGIN;",
                        f"INSERT INTO t VALUES ({key}, {tid});",
                        "ROLLBACK;",
                        f"SELECT id FROM t WHERE id = {key};",
                    ], read_timeout=0.5)
                    if "Error:" in out or "timeout" in out.lower():
                        errors.append(f"tid={tid} i={i} out={out}")
                        return
                    # After rollback the row must be gone; only the header /
                    # empty result is acceptable — never "key |" content.
                    if f"{key} |" in out.split("ROLLBACK", 1)[-1]:
                        errors.append(f"aborted row visible tid={tid} key={key}\n{out}")
                        return
            except Exception as exc:  # noqa: BLE001
                errors.append(f"tid={tid}: {exc}")

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(workers)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=120)
        if errors:
            raise AssertionError(f"abort visibility errors seed={seed}: {errors[:3]}")

        rows = select_rows_from_output(
            server.execute(["SELECT COUNT(*) FROM t;"], read_timeout=2.0)
        )
        if rows != [("1",)]:
            raise AssertionError(f"expected only seed row, got {rows} seed={seed}")

        print(f"abort_visibility PASS seed={seed} workers={workers} rounds={rounds}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"abort_visibility FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop()
        cleanup(db_dir)


if __name__ == "__main__":
    sys.exit(main())
