#!/usr/bin/env python3
"""Checkpoint must not truncate WAL while a transaction is still open.

If dirty uncommitted pages are flushed and the WAL is discarded, crash
recovery treats those rows as committed. Background checkpoints with a
short timeout must keep the WAL until active transactions drain.
"""

from __future__ import annotations

import argparse
import os
import socket
import sys
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
    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(1.0)
        self.sock.recv(4096)

    def exec(self, sql: str, idle: float = 0.4) -> str:
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


def write_aggressive_config(db_dir: str) -> None:
    with open(os.path.join(db_dir, "minidb.conf"), "w") as f:
        f.write(
            "checkpoint_timeout = 50ms\n"
            "checkpoint_wal_size = 64KB\n"
            "wal_group_commit = on\n"
            "wal_group_commit_delay = 1ms\n"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db_dir = temp_db("minidb-ckpt-open-txn.")
    port = 18400 + (os.getpid() % 1000)
    write_aggressive_config(db_dir)
    server = TcpMiniDB(args.bin, db_dir, port)
    holder = None
    try:
        setup = run_minidb(args.bin, db_dir, [
            "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
            "INSERT INTO t VALUES (1, 10);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()
        holder = Session(port)
        out = holder.exec("BEGIN;")
        if "Error" in out:
            raise AssertionError(f"BEGIN failed: {out}")
        out = holder.exec("INSERT INTO t VALUES (999, 99);")
        if "Error" in out:
            raise AssertionError(f"INSERT failed: {out}")

        # Let the background checkpoint loop run while the txn stays open.
        time.sleep(1.5 if not args.stress else 4.0)

        # Crash without COMMIT — recovery must undo the open INSERT even if
        # its pages were flushed by checkpoint.
        server.stop(kill=True)
        holder = None

        rows = minidb_query(args.bin, db_dir, "SELECT id, v FROM t ORDER BY id;", seed)
        if rows != [("1", "10")]:
            raise AssertionError(
                f"open-txn row survived checkpoint+crash seed={seed}: {rows}"
            )
        print(f"checkpoint_open_txn PASS seed={seed}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"checkpoint_open_txn FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        if holder is not None:
            try:
                holder.close()
            except OSError:
                pass
        server.stop(kill=True)
        cleanup(db_dir)


if __name__ == "__main__":
    sys.exit(main())
