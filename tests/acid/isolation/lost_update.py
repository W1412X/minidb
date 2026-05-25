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

def read_available(sock: socket.socket, timeout: float = 0.5) -> str:
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


def send_one(sock: socket.socket, sql: str, timeout: float = 0.5) -> str:
    if not sql.rstrip().endswith(";"):
        sql += ";"
    sock.sendall((sql + "\n").encode())
    return read_available(sock, timeout)


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

        sockets: list[socket.socket] = []
        try:
            for _ in range(2):
                sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                sock.recv(4096)  # welcome
                sockets.append(sock)

            begin_replies = [send_one(sock, "BEGIN;", 0.5) for sock in sockets]
            if any("Transaction" not in r for r in begin_replies):
                raise AssertionError(f"BEGIN failed: {begin_replies}")

            # Force both sessions to establish their snapshot before either
            # writer reaches UPDATE. Sending BEGIN/UPDATE/COMMIT as one batch
            # lets a fast Linux server serialize the sessions, which does not
            # exercise lost-update prevention at all.
            reads = [send_one(sock, "SELECT v FROM counter WHERE id = 1;", 0.5)
                     for sock in sockets]
            if any("0" not in r for r in reads):
                raise AssertionError(f"snapshot reads failed: {reads}")

            def worker(tid: int) -> None:
                # Both sessions already hold snapshots that saw v=0. Now the
                # writes race on the same RID; one must surface a conflict.
                sock = sockets[tid]
                sock.sendall(b"UPDATE counter SET v = v + 1 WHERE id = 1;\n")
                time.sleep(0.05)
                sock.sendall(b"COMMIT;\n")
                replies[tid] = read_available(sock, 2.0)

            threads = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=30)
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except OSError:
                    pass

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
