#!/usr/bin/env python3
"""Checkpoint-barrier durability test for ACID item D2.

Background workers commit rows while a low checkpoint_timeout forces
frequent CHECKPOINTs in the background. The server is then SIGKILLed
without a clean shutdown, the database is reopened, and the test
asserts that every acknowledged "Transaction committed." row is
present after recovery.

Before D2: BufferPool::flush_all() ran BEFORE WalManager::checkpoint(),
so a concurrent writer could dirty a new page whose WAL record was
then truncated together with the rest of the log. After SIGKILL +
restart, the committed row would silently vanish.

After D2: the page flush runs INSIDE WalManager::checkpoint() while
the WAL latch is still held, so no writer can sneak a record into the
about-to-be-truncated window.
"""

from __future__ import annotations

import argparse
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    TcpMiniDB,
    add_seed_args,
    cleanup,
    minidb_query,
    run_minidb,
    temp_db,
)


def write_aggressive_config(db_dir: str) -> None:
    """Force a checkpoint at least every 50 ms or 128 KB of WAL."""
    with open(os.path.join(db_dir, "minidb.conf"), "w") as f:
        f.write(
            "checkpoint_timeout = 50ms\n"
            "checkpoint_wal_size = 128KB\n"
            "wal_group_commit = on\n"
            "wal_group_commit_delay = 1ms\n"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    workers = 4 if not args.stress else 12
    per_worker = args.cases or (60 if not args.stress else 400)
    db_dir = temp_db("minidb-d2-barrier.")
    port = 18100 + (os.getpid() % 1000)
    write_aggressive_config(db_dir)
    server = TcpMiniDB(args.bin, db_dir, port)

    committed_keys: set[int] = set()
    committed_lock = threading.Lock()
    errors: list[str] = []
    stop = threading.Event()

    try:
        setup = run_minidb(args.bin, db_dir, [
            "CREATE TABLE chk (id INT PRIMARY KEY, v INT);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()

        def worker(tid: int) -> None:
            for i in range(per_worker):
                if stop.is_set():
                    return
                key = 1_000_000 + tid * 100_000 + i
                reply = server.execute([
                    "BEGIN;",
                    f"INSERT INTO chk VALUES ({key}, {tid});",
                    "COMMIT;",
                ], read_timeout=0.3)
                if "Error:" in reply:
                    continue
                if "Transaction committed." in reply:
                    with committed_lock:
                        committed_keys.add(key)
                elif "Transaction rolled back." not in reply:
                    errors.append(f"tid={tid} key={key} reply={reply!r}")

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(workers)]
        for t in threads:
            t.start()

        # Let workers and the background checkpoint loop interleave for a
        # bit, then SIGKILL the server so any race between dirty-page
        # flush and WAL truncate is exposed.
        time.sleep(2.0 if not args.stress else 5.0)
        stop.set()
        server.stop(kill=True)
        for t in threads:
            t.join(timeout=5)

        if errors:
            raise AssertionError(f"worker errors seed={seed}: {errors[:3]}")

        rows = minidb_query(args.bin, db_dir,
                            "SELECT id FROM chk ORDER BY id;", seed, timeout=30)
        persisted = {int(r[0]) for r in rows}
        missing = committed_keys - persisted
        if missing:
            raise AssertionError(
                f"D2 violation seed={seed}: {len(missing)} acknowledged commits "
                f"missing after SIGKILL+restart. Sample: {sorted(missing)[:10]}"
            )

        print(f"checkpoint_barrier PASS seed={seed} acknowledged={len(committed_keys)}")
        return 0
    except Exception as exc:
        print(f"checkpoint_barrier FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        stop.set()
        server.stop(kill=True)
        cleanup(db_dir)


if __name__ == "__main__":
    raise SystemExit(main())
