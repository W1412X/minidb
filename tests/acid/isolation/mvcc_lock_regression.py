#!/usr/bin/env python3
"""MVCC, lock, and multi-connection regression coverage."""

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
    minidb_query,
    run_minidb,
    seeded_rng,
    select_rows_from_output,
    temp_db,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    rng = seeded_rng(seed)
    workers = 2 if not args.stress else 32
    per_worker = args.cases or (3 if not args.stress else 200)
    db_dir = temp_db("minidb-mvcc-lock.")
    port = 17000 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db_dir, port)
    try:
        out = run_minidb(args.bin, db_dir, [
            "CREATE TABLE tx (id INT PRIMARY KEY, v INT, owner INT);",
            "INSERT INTO tx VALUES (1, 10, 0);",
        ])
        assert_no_error(out, seed, "mvcc setup")

        rollback = run_minidb(args.bin, db_dir, [
            "BEGIN;",
            "INSERT INTO tx VALUES (2, 20, 0);",
            "UPDATE tx SET v = 99 WHERE id = 1;",
            "DELETE FROM tx WHERE id = 1;",
            "ROLLBACK;",
        ])
        assert_no_error(rollback, seed, "rollback")
        assert minidb_query(args.bin, db_dir, "SELECT id, v FROM tx ORDER BY id;", seed) == [("1", "10")]

        server.start()
        errors: list[str] = []

        def worker(tid: int) -> None:
            local_rng = seeded_rng(seed + tid)
            sqls: list[str] = []
            for i in range(per_worker):
                key = 100000 + tid * 1000 + i
                val = local_rng.randint(1, 10000)
                sqls.extend([
                    "BEGIN;",
                    f"INSERT INTO tx VALUES ({key}, {val}, {tid});",
                    f"UPDATE tx SET v = v + 1 WHERE id = {key};",
                    "COMMIT;",
                ])
            out = server.execute(sqls, read_timeout=5.0)
            if "Error:" in out or "timeout" in out:
                errors.append(f"tid={tid} output={out}")

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(workers)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=60 if not args.stress else 300)
        if errors:
            raise AssertionError(f"concurrent worker errors seed={seed}: {errors[:3]}")

        expected = 1 + workers * per_worker
        rows = select_rows_from_output(server.execute(["SELECT COUNT(*) FROM tx;"], read_timeout=2.0))
        if rows != [(str(expected),)]:
            raise AssertionError(f"count mismatch seed={seed}: {rows} expected={expected}")

        out = server.execute([
            "BEGIN;",
            "UPDATE tx SET v = 123 WHERE id = 1;",
            "ROLLBACK;",
            "SELECT id, v FROM tx WHERE id = 1;",
        ])
        assert_no_error(out, seed, "server rollback")
        if "1 | 10" not in out:
            raise AssertionError(f"server rollback visibility failed seed={seed}\n{out}")

        print(f"mvcc_lock_regression PASS seed={seed} workers={workers} per_worker={per_worker}")
        return 0
    except Exception as exc:
        print(f"mvcc_lock_regression FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=False)
        cleanup(db_dir)


if __name__ == "__main__":
    raise SystemExit(main())
