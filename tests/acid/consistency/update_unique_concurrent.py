#!/usr/bin/env python3
"""C4: concurrent UPDATEs must not violate UNIQUE / PRIMARY KEY.

Two sessions update different rows to the same unique value. Snapshot
isolation alone lets both pass a pre-write uniqueness check; logical
unique-key locks (same as INSERT) must serialize them so exactly one
wins and the other fails or retries without leaving duplicates.
"""

from __future__ import annotations

import argparse
import os
import socket
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


def read_available(sock: socket.socket, timeout: float = 1.0) -> str:
    result = b""
    sock.settimeout(timeout)
    while True:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            result += chunk
        except socket.timeout:
            break
    return result.decode(errors="replace")


def send_one(sock: socket.socket, sql: str, timeout: float = 1.0) -> str:
    if not sql.rstrip().endswith(";"):
        sql += ";"
    sock.sendall((sql + "\n").encode())
    return read_available(sock, timeout)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-upd-unique.")
    port = 18200 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, u INT UNIQUE, v INT);",
            "INSERT INTO t VALUES (1, 10, 0), (2, 20, 0);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()
        replies: dict[int, str] = {}
        sockets: list[socket.socket] = []
        try:
            for _ in range(2):
                sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                sock.recv(4096)
                sockets.append(sock)

            for sock in sockets:
                send_one(sock, "BEGIN;", 0.5)

            # Pin snapshots before either UPDATE runs.
            for sock in sockets:
                send_one(sock, "SELECT id FROM t ORDER BY id;", 0.5)

            barrier = threading.Barrier(2)

            def worker(tid: int) -> None:
                sock = sockets[tid]
                # Both try to claim unique value 99 on different rows.
                rid = 1 if tid == 0 else 2
                barrier.wait(timeout=5)
                replies[tid] = send_one(
                    sock, f"UPDATE t SET u = 99 WHERE id = {rid};", 5.0
                )
                if "Error" in replies[tid] or "serialize" in replies[tid].lower():
                    send_one(sock, "ROLLBACK;", 1.0)
                else:
                    send_one(sock, "COMMIT;", 2.0)

            threads = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=60)

            # Give the server a moment, then verify uniqueness on a fresh read.
            # Under key locks, at most one row may hold u=99; duplicates are
            # the failure mode this test exists to catch.
            time.sleep(0.2)
            rows = minidb_query(
                args.bin, db, "SELECT id, u FROM t ORDER BY id;", seed
            )
            uniq = [u for _, u in rows]
            if len(uniq) != len(set(uniq)):
                raise AssertionError(
                    f"duplicate unique values after concurrent UPDATE: {rows} replies={replies}"
                )
            if uniq.count("99") != 1:
                raise AssertionError(
                    f"expected exactly one row with u=99, got {rows} replies={replies}"
                )

            print(f"update_unique_concurrent PASS seed={seed} replies={replies}")
            return 0
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except OSError:
                    pass
    except Exception as exc:  # noqa: BLE001
        print(f"update_unique_concurrent FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop()
        cleanup(db)


if __name__ == "__main__":
    sys.exit(main())
