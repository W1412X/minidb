#!/usr/bin/env python3
"""Statement-level atomicity in explicit transactions (ACID A2).

Before this fix, a mid-statement failure inside BEGIN..COMMIT left
the writes from earlier rows of the same statement in place. If the
user then COMMITed, those half-rows persisted; if they ROLLBACKed the
whole txn was wiped — but there was no way to recover at the
statement boundary.

The new TransactionManager::rollback_to_savepoint() lets the REPL and
the server roll back exactly this statement's writes while keeping
the transaction alive. The undo log is rewound; the txn stays ACTIVE.

This test asserts:

  1. A multi-row INSERT whose middle row violates NOT NULL leaves NO
     rows from that statement (all the earlier rows in the same
     INSERT are rolled back too).
  2. The transaction is still alive afterwards — a follow-up INSERT
     in the same txn succeeds and a COMMIT persists it.
  3. Earlier statements in the same txn are NOT rolled back.
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
    """Single TCP connection for stable transaction state across calls."""

    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(0.3)
        self.sock.recv(4096)

    def exec(self, sql: str) -> str:
        if not sql.endswith(";"):
            sql += ";"
        self.sock.sendall((sql + "\n").encode())
        buf = b""
        while True:
            try:
                c = self.sock.recv(4096)
                if not c:
                    break
                buf += c
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
    db = temp_db("minidb-savepoint.")
    port = 18310 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16) NOT NULL);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")
        server.start()

        s = Session(port)
        try:
            s.exec("BEGIN")
            s.exec("INSERT INTO t VALUES (1, 'alpha')")
            # Mid-statement failure: row 2 OK, row 3 violates NOT NULL.
            bad = s.exec("INSERT INTO t VALUES (2, 'beta'), (3, NULL), (4, 'delta')")
            if "Error" not in bad:
                raise AssertionError(f"expected NOT NULL error, got: {bad}")
            # Statement-level rollback: row 2 must NOT have leaked.
            mid = s.exec("SELECT id FROM t ORDER BY id")
            if "1" not in mid:
                raise AssertionError(
                    f"earlier statement's row missing — txn-wide rollback? {mid}")
            if "2" in mid or "3" in mid or "4" in mid:
                raise AssertionError(
                    f"partial INSERT leaked rows past the savepoint: {mid}")

            # Transaction is still alive — a follow-up succeeds.
            ok = s.exec("INSERT INTO t VALUES (5, 'eps')")
            if "Error" in ok:
                raise AssertionError(
                    f"txn should still be ACTIVE after savepoint rollback: {ok}")
            s.exec("COMMIT")
        finally:
            s.close()

        # After commit: rows 1 + 5 visible; rows 2/3/4 must NOT be there.
        rows = minidb_query(args.bin, db, "SELECT id FROM t ORDER BY id;", seed)
        ids = [r[0] for r in rows]
        if ids != ["1", "5"]:
            raise AssertionError(
                f"final state wrong (seed={seed}): expected ['1','5'], got {ids}")

        # And one more sub-test: UPDATE inside explicit txn that hits a
        # mid-statement NOT NULL must roll back to the savepoint, leaving
        # earlier-statement updates intact.
        cleanup(db)
        db2 = temp_db("minidb-savepoint-update.")
        port2 = port + 1
        server2 = TcpMiniDB(args.bin, db2, port2)
        try:
            setup = run_minidb(args.bin, db2, [
                "CREATE TABLE u (id INT PRIMARY KEY, name VARCHAR(16) NOT NULL);",
                "INSERT INTO u VALUES (1, 'a'), (2, 'b'), (3, 'c');",
            ])
            if "Error" in setup:
                raise AssertionError(f"setup u failed: {setup}")
            server2.start()
            s2 = Session(port2)
            try:
                s2.exec("BEGIN")
                s2.exec("UPDATE u SET name = 'X' WHERE id = 1")
                # Try a SET name = NULL — every matched row violates NOT
                # NULL. The first match aborts the statement.
                bad = s2.exec("UPDATE u SET name = NULL WHERE id >= 2")
                if "Error" not in bad:
                    raise AssertionError(f"expected NOT NULL error: {bad}")
                # Statement A's update to id=1 must still be visible.
                snap = s2.exec("SELECT id, name FROM u ORDER BY id")
                if "1 | X" not in snap:
                    raise AssertionError(
                        f"earlier UPDATE was wiped by mid-statement error: {snap}")
                s2.exec("COMMIT")
            finally:
                s2.close()
            rows2 = minidb_query(args.bin, db2,
                                 "SELECT id, name FROM u ORDER BY id;", seed)
            if rows2 != [("1", "X"), ("2", "b"), ("3", "c")]:
                raise AssertionError(
                    f"update-final wrong (seed={seed}): {rows2}")
        finally:
            server2.stop(kill=True)
            cleanup(db2)

        print(f"statement_savepoint PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"statement_savepoint FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
