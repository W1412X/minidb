#!/usr/bin/env python3
"""Serializable isolation eliminates the doctors-on-call write skew (I3+).

Under Snapshot Isolation (`SET ISOLATION_LEVEL = SNAPSHOT`, the default),
the workload in tests/acid/isolation/write_skew.py shows the classic
write-skew anomaly: both Tx A and Tx B commit and the on-call invariant
ends up violated.

Under Serializable (`SET ISOLATION_LEVEL = SERIALIZABLE`) MiniDB now
runs SSI-lite: every visible row a serializable transaction reads goes
into its read set, and at commit time the read set is intersected
against the write sets of all transactions that committed during our
lifetime. Any overlap aborts us so the surviving txns serialize in
commit order.

Test fixture:
  doctors(id, on_call) starts with 2 rows, both on_call=1.
  Tx A   SELECT * FROM doctors;   (read set = {alice_rid, bob_rid})
  Tx A   UPDATE doctors SET on_call=0 WHERE id=1;
  Tx B   SELECT * FROM doctors;   (read set = {alice_rid, bob_rid})
  Tx B   UPDATE doctors SET on_call=0 WHERE id=2;
  Tx A   COMMIT;   (first committer wins; history += A's writes)
  Tx B   COMMIT;   (SSI conflict — A wrote alice_rid, B read it -> abort)

We pin: exactly one transaction commits, and at least one doctor is
still on call afterwards.
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


class Session:
    """Persistent TCP session — keeps BEGIN..COMMIT on one connection."""

    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(2)
        # consume welcome banner
        self.sock.recv(4096)

    def send(self, sql: str, read_timeout: float = 2.0) -> str:
        if not sql.rstrip().endswith(";"):
            sql += ";"
        self.sock.sendall((sql + "\n").encode())
        self.sock.settimeout(read_timeout)
        buf = b""
        end = time.time() + read_timeout
        while time.time() < end:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
                # Heuristic: most replies are short and contain a newline at end.
                if buf.endswith(b"\n"):
                    # Allow a short tail in case more data is en-route.
                    self.sock.settimeout(0.05)
                    try:
                        more = self.sock.recv(4096)
                        if more:
                            buf += more
                            continue
                    except (socket.timeout, OSError):
                        pass
                    break
            except socket.timeout:
                break
        return buf.decode(errors="replace")

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-ws-serial.")
    port = 18500 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE doctors (id INT PRIMARY KEY, on_call INT);",
            "INSERT INTO doctors VALUES (1, 1);",
            "INSERT INTO doctors VALUES (2, 1);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()

        # The isolation switch is process-wide on the TransactionManager.
        # One quick session sets it; subsequent worker sessions inherit
        # serializable at BEGIN time.
        ctrl = Session(port)
        try:
            ack = ctrl.send("SET ISOLATION_LEVEL = SERIALIZABLE;")
            if "SET" not in ack:
                raise AssertionError(f"could not switch to serializable: {ack}")
        finally:
            ctrl.close()

        replies: dict[int, str] = {"prep": {}, "commit": {}}
        prepared = threading.Barrier(2)
        commit_gate = threading.Barrier(2)

        def worker(tid: int) -> None:
            target = 1 if tid == 0 else 2
            sess = Session(port)
            try:
                buf = sess.send("BEGIN;")
                buf += sess.send("SELECT * FROM doctors;")
                buf += sess.send(f"UPDATE doctors SET on_call = 0 WHERE id = {target};")
                replies["prep"][tid] = buf
                prepared.wait()
                commit_gate.wait()
                replies["commit"][tid] = sess.send("COMMIT;")
            finally:
                sess.close()

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)
        # Flatten for the assertions below.
        replies = {tid: replies["prep"].get(tid, "") + replies["commit"].get(tid, "")
                   for tid in [0, 1]}

        # Exactly one transaction must have committed; the other must report
        # an error or "no active transaction" (because rollback closed it).
        committed = 0
        aborted = 0
        for tid, body in replies.items():
            if "Transaction committed." in body:
                committed += 1
            elif ("Error" in body or "no active transaction" in body):
                aborted += 1
        if committed != 1 or aborted != 1:
            raise AssertionError(
                f"expected exactly 1 commit + 1 abort, got "
                f"committed={committed} aborted={aborted} replies={replies}")

        rows = minidb_query(args.bin, db,
                            "SELECT id, on_call FROM doctors ORDER BY id;", seed)
        on_call = [r for r in rows if r[1] == "1"]
        if not on_call:
            raise AssertionError(
                f"serializable abort failed to preserve invariant: rows={rows}")

        print(f"write_skew_serializable PASS seed={seed} "
              f"(one txn aborted, invariant preserved)")
        return 0
    except Exception as exc:
        print(f"write_skew_serializable FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
