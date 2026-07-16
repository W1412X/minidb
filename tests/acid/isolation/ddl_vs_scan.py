#!/usr/bin/env python3
"""DDL must wait for scans/cursors that hold AccessShare (no UAF).

DECLARE CURSOR keeps AccessShare until CLOSE. Concurrent DROP TABLE must
block until the cursor is closed, then succeed — never crash the server.
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
    run_minidb,
    temp_db,
)


class Session:
    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(2.0)
        self.sock.recv(4096)

    def exec(self, sql: str, idle: float = 1.0) -> str:
        if not sql.endswith(";"):
            sql += ";"
        self.sock.sendall((sql + "\n").encode())
        self.sock.settimeout(idle)
        buf = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
            except socket.timeout:
                break
        return buf.decode(errors="replace")

    def close(self) -> None:
        try:
            self.sock.sendall(b"exit;\n")
        except OSError:
            pass
        self.sock.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db_dir = temp_db("minidb-ddl-vs-scan.")
    port = 18500 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db_dir, port)
    cursor_sess = None
    try:
        setup = run_minidb(args.bin, db_dir, [
            "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
            "INSERT INTO t VALUES (1, 10);",
            "INSERT INTO t VALUES (2, 20);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()
        cursor_sess = Session(port)
        out = cursor_sess.exec("DECLARE c CURSOR FOR SELECT id, v FROM t;")
        if "Error" in out:
            raise AssertionError(f"DECLARE failed: {out}")

        drop_reply: list[str] = []
        drop_done = threading.Event()

        def dropper() -> None:
            try:
                s = Session(port)
                # Must wait for AccessShare held by the cursor txn.
                reply = s.exec("DROP TABLE t;", idle=8.0)
                drop_reply.append(reply)
                s.close()
            except Exception as exc:  # noqa: BLE001
                drop_reply.append(f"EXC:{exc}")
            finally:
                drop_done.set()

        t = threading.Thread(target=dropper)
        t.start()
        time.sleep(0.4)
        if drop_done.is_set():
            raise AssertionError(
                f"DROP finished while cursor still open: {drop_reply}"
            )

        fetch = cursor_sess.exec("FETCH 10 FROM c;")
        if "Error" in fetch:
            raise AssertionError(f"FETCH under cursor failed: {fetch}")
        if "1 |" not in fetch and "1\n" not in fetch and "| 1" not in fetch:
            # Header + rows; accept either pipe or plain format.
            if "10" not in fetch:
                raise AssertionError(f"FETCH missing rows: {fetch}")

        close = cursor_sess.exec("CLOSE c;")
        if "Error" in close:
            raise AssertionError(f"CLOSE failed: {close}")

        if not drop_done.wait(10.0):
            raise AssertionError("DROP did not complete after CLOSE")
        t.join(timeout=5)

        reply = drop_reply[0] if drop_reply else ""
        if "Error" in reply and "dropped" not in reply.lower():
            raise AssertionError(f"DROP failed after CLOSE: {reply}")
        if server.proc and server.proc.poll() is not None:
            raise AssertionError("server died during ddl vs cursor race")

        # Table should be gone.
        probe = Session(port)
        gone = probe.exec("SELECT id FROM t;")
        probe.close()
        if "Error" not in gone and "not found" not in gone.lower():
            raise AssertionError(f"table still queryable after DROP: {gone}")

        print(f"ddl_vs_scan PASS seed={seed}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"ddl_vs_scan FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        if cursor_sess is not None:
            try:
                cursor_sess.close()
            except OSError:
                pass
        server.stop(kill=True)
        cleanup(db_dir)


if __name__ == "__main__":
    sys.exit(main())
