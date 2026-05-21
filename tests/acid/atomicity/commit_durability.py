#!/usr/bin/env python3
"""Concurrent commit durability test for ACID item A1/D3.

For every connection that observes a successful COMMIT reply, the row it
inserted must be present after restart. This exercises:

  - group-commit follower lost-wakeup safety,
  - flush_commit returning false on durability failure,
  - the post-fsync slot-flip ordering in TransactionManager::commit.

The test runs concurrent BEGIN/INSERT/COMMIT cycles, restarts the database,
and compares the set of "Transaction committed."-acknowledged rows against
the set persisted on disk.
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
    minidb_query,
    run_minidb,
    seeded_rng,
    temp_db,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    rng = seeded_rng(seed)

    workers = 4 if not args.stress else 16
    per_worker = args.cases or (20 if not args.stress else 200)
    db_dir = temp_db("minidb-commit-durability.")
    port = 17800 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db_dir, port)

    committed_keys: set[int] = set()
    committed_lock = threading.Lock()
    errors: list[str] = []

    try:
        setup = run_minidb(args.bin, db_dir, [
            "CREATE TABLE commit_t (id INT PRIMARY KEY, owner INT);",
        ])
        assert_no_error(setup, seed, "setup")

        server.start()

        def worker(tid: int) -> None:
            local_rng = seeded_rng(seed + tid)
            for i in range(per_worker):
                key = 1_000_000 + tid * 100_000 + i
                # Re-seed-derived value so writes do not look identical.
                owner = local_rng.randint(1, 1_000_000)
                reply = server.execute([
                    "BEGIN;",
                    f"INSERT INTO commit_t VALUES ({key}, {owner});",
                    "COMMIT;",
                ], read_timeout=0.3)
                if "Error:" in reply:
                    # Acceptable: a unique violation or resource cap. We
                    # only commit-track keys that the server acknowledged.
                    continue
                if "Transaction committed." in reply:
                    with committed_lock:
                        committed_keys.add(key)
                elif "Transaction rolled back." in reply:
                    continue
                else:
                    errors.append(f"tid={tid} key={key} unexpected reply: {reply!r}")

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(workers)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=120 if args.stress else 60)

        if errors:
            raise AssertionError(f"worker errors seed={seed}: {errors[:3]}")

        server.stop()

        rows = minidb_query(args.bin, db_dir, "SELECT id FROM commit_t ORDER BY id;",
                            seed, timeout=30)
        persisted = {int(r[0]) for r in rows}

        missing = committed_keys - persisted
        if missing:
            raise AssertionError(
                f"durability violation seed={seed}: {len(missing)} acknowledged "
                f"commits not persisted. Sample: {sorted(missing)[:10]}"
            )

        # Persisted rows beyond the acknowledged set would also be a bug
        # (a commit reported failure but landed on disk). We accept it for
        # rolled-back paths but flag it for clearly unexpected keys.
        unexpected = persisted - committed_keys
        if unexpected:
            raise AssertionError(
                f"phantom durability seed={seed}: {len(unexpected)} unacknowledged "
                f"rows persisted. Sample: {sorted(unexpected)[:10]}"
            )

        print(f"commit_durability PASS seed={seed} acknowledged={len(committed_keys)}")
        return 0
    except Exception as exc:
        print(f"commit_durability FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db_dir)


if __name__ == "__main__":
    raise SystemExit(main())
