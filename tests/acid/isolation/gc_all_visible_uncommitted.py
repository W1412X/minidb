#!/usr/bin/env python3
"""C3: GC/VACUUM must not mark pages all-visible while uncommitted tuples exist.

IndexOnlyScan skips heap MVCC when the visibility map says a page is
all-visible. If GC sets that bit merely because it found no garbage, a
concurrent IndexOnlyScan can return keys from an in-flight INSERT.
"""

from __future__ import annotations

import argparse
import os
import socket
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    TcpMiniDB,
    add_seed_args,
    cleanup,
    run_minidb,
    temp_db,
)


class Session:
    def __init__(self, port: int, idle: float = 0.3) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(1.0)
        self.sock.recv(4096)
        self.idle = idle

    def exec(self, sql: str) -> str:
        if not sql.endswith(";"):
            sql += ";"
        self.sock.sendall((sql + "\n").encode())
        self.sock.settimeout(self.idle)
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
    db = temp_db("minidb-gc-allvis.")
    port = 18300 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    writer = None
    reader = None
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, pad VARCHAR(8));",
            "INSERT INTO t VALUES (1, 'a');",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()
        writer = Session(port)
        reader = Session(port)

        w = writer.exec("BEGIN;")
        if "Error" in w:
            raise AssertionError(f"BEGIN failed: {w}")
        w = writer.exec("INSERT INTO t VALUES (2, 'b');")
        if "Error" in w:
            raise AssertionError(f"INSERT failed: {w}")

        # VACUUM / GC while the insert is still in-flight.
        v = reader.exec("VACUUM;")
        if "Error" in v:
            raise AssertionError(f"VACUUM failed: {v}")

        # PK equality / covering projection should prefer an index path.
        # Uncommitted id=2 must not appear to this snapshot.
        out = reader.exec("SELECT id FROM t WHERE id = 2;")
        if "Error" in out:
            raise AssertionError(f"SELECT failed: {out}")
        # Result rows look like "2 |" or a bare "2" after the header.
        body = out
        if "2 |" in body or "\n2\n" in body or body.strip().endswith("\n2"):
            # Also accept header-only false positives carefully: require a
            # data cell, not just the digit inside "id".
            lines = [ln.strip() for ln in body.splitlines() if ln.strip()]
            data_lines = [ln for ln in lines if ln[0].isdigit()]
            if any(ln.startswith("2") for ln in data_lines):
                raise AssertionError(
                    f"uncommitted row visible after VACUUM (VM dirty read?): {out}"
                )

        cnt = reader.exec("SELECT COUNT(*) FROM t;")
        if "2 |" in cnt or "\n2\n" in cnt:
            # COUNT should be 1 (only the committed seed row).
            lines = [ln.strip() for ln in cnt.splitlines() if ln.strip()]
            data = [ln for ln in lines if ln[0].isdigit()]
            if data and data[-1].startswith("2"):
                raise AssertionError(f"COUNT saw uncommitted insert: {cnt}")

        writer.exec("ROLLBACK;")
        print(f"gc_all_visible_uncommitted PASS seed={seed}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"gc_all_visible_uncommitted FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        if writer:
            writer.close()
        if reader:
            reader.close()
        server.stop()
        cleanup(db)


if __name__ == "__main__":
    sys.exit(main())
