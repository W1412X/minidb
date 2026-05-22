#!/usr/bin/env python3
"""Exhaustive MVCC visibility matrix (ACID C2).

For a tuple version V with (xmin, xmax) and a reader transaction T with
snapshot id S, snapshot-isolation visibility is:

  visible iff
      xmin is committed AND xmin < S AND xmin not in active(S)
      AND (xmax == 0 OR xmax not committed OR xmax >= S OR xmax was T)

This test sweeps every meaningful (xmin-state, xmax-state) combination
plus the own-read / own-delete corners. Each case is checked twice —
once via SeqScan (WHERE on a non-indexed VARCHAR column) and once via
IndexScan (WHERE on the primary key). Both paths must agree on what
the active snapshot sees, since DELETE / UPDATE now leave the index
entry in place and rely on the heap-side visibility check to filter.

A "session" here is one TCP connection so BEGIN / SELECT / COMMIT are
all carried over the same channel and the snapshot pinned at BEGIN
sticks for the lifetime of the session.
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
    run_minidb,
    temp_db,
)


class Session:
    """A single TCP connection to the MiniDB server.

    Each `exec()` call sends a statement and reads until the socket
    quiesces. Reading-by-timeout is acceptable because all assertions
    in this test wait deterministically for a quiet period (<200 ms).
    """

    def __init__(self, port: int, idle: float = 0.15) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(1.0)
        self.sock.recv(4096)  # greeting / banner
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


def select_count(session: Session, where: str) -> int:
    """SELECT COUNT(*) ... — return the integer."""
    out = session.exec(f"SELECT COUNT(*) FROM mvcc WHERE {where}")
    for raw in out.splitlines():
        line = raw.strip()
        if line.isdigit():
            return int(line)
    raise AssertionError(f"could not parse COUNT(*) reply: {out!r}")


def select_via_seqscan(session: Session, key: int) -> int:
    # `pad` is non-indexed → SeqScan + Filter, the heap-recheck path.
    return select_count(session, f"pad = 'pad_{key}'")


def select_via_indexscan(session: Session, key: int) -> int:
    # `id` is the primary key → IndexScan walks the version chain and
    # applies the same is_visible check that SeqScan does.
    return select_count(session, f"id = {key}")


def assert_visible(session: Session, key: int, expected: int, ctx: str,
                   seed: int) -> None:
    got_seq = select_via_seqscan(session, key)
    if got_seq != expected:
        raise AssertionError(
            f"[{ctx}] seq-scan visibility mismatch for key={key}: "
            f"got {got_seq}, expected {expected} (seed={seed})"
        )
    got_idx = select_via_indexscan(session, key)
    if got_idx != expected:
        raise AssertionError(
            f"[{ctx}] index-scan visibility mismatch for key={key}: "
            f"got {got_idx}, expected {expected} (seed={seed})"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-mvcc-matrix.")
    port = 18200 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE mvcc (id INT PRIMARY KEY, pad VARCHAR(16));",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")
        server.start()

        # ============================================================
        # Case A1 — xmin committed before T, xmax == 0 → visible.
        # ============================================================
        a = Session(port)
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (1, 'pad_1')")
        a.exec("COMMIT")

        t = Session(port)
        t.exec("BEGIN")
        assert_visible(t, 1, 1, "A1", seed)
        t.exec("COMMIT")

        # ============================================================
        # Case A2 — xmin committed before T, xmax committed before T → invisible.
        # ============================================================
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (2, 'pad_2')")
        a.exec("COMMIT")
        a.exec("BEGIN")
        a.exec("DELETE FROM mvcc WHERE id = 2")
        a.exec("COMMIT")

        t = Session(port)
        t.exec("BEGIN")
        assert_visible(t, 2, 0, "A2", seed)
        t.exec("COMMIT")

        # ============================================================
        # Case A3 — xmin before T, xmax committed AFTER T → still visible.
        # ============================================================
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (3, 'pad_3')")
        a.exec("COMMIT")

        t = Session(port)
        t.exec("BEGIN")          # T's snapshot taken NOW
        a.exec("BEGIN")
        a.exec("DELETE FROM mvcc WHERE id = 3")
        a.exec("COMMIT")         # commits after T's snapshot
        assert_visible(t, 3, 1, "A3", seed)
        t.exec("COMMIT")

        # ============================================================
        # Case A4 — delete rolled back → row stays visible.
        # ============================================================
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (4, 'pad_4')")
        a.exec("COMMIT")
        a.exec("BEGIN")
        a.exec("DELETE FROM mvcc WHERE id = 4")
        a.exec("ROLLBACK")

        t = Session(port)
        t.exec("BEGIN")
        assert_visible(t, 4, 1, "A4", seed)
        t.exec("COMMIT")

        # ============================================================
        # Case A5 — xmax set by another *active* txn → still visible.
        # T's read must not block; SI MVCC reads do not take row locks.
        # ============================================================
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (5, 'pad_5')")
        a.exec("COMMIT")

        a.exec("BEGIN")
        a.exec("DELETE FROM mvcc WHERE id = 5")    # uncommitted

        t = Session(port)
        t.exec("BEGIN")
        assert_visible(t, 5, 1, "A5", seed)
        t.exec("COMMIT")
        a.exec("ROLLBACK")

        # ============================================================
        # Case B1 — own insert is visible to self before commit.
        # ============================================================
        t = Session(port)
        t.exec("BEGIN")
        t.exec("INSERT INTO mvcc VALUES (6, 'pad_6')")
        assert_visible(t, 6, 1, "B1", seed)
        t.exec("ROLLBACK")

        # ============================================================
        # Case B2 — own delete hides the row from self.
        # ============================================================
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (7, 'pad_7')")
        a.exec("COMMIT")
        t = Session(port)
        t.exec("BEGIN")
        t.exec("DELETE FROM mvcc WHERE id = 7")
        assert_visible(t, 7, 0, "B2", seed)
        t.exec("ROLLBACK")

        # ============================================================
        # Case C — another transaction's uncommitted insert is invisible.
        # ============================================================
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (8, 'pad_8')")
        t = Session(port)
        t.exec("BEGIN")
        assert_visible(t, 8, 0, "C", seed)
        t.exec("COMMIT")
        a.exec("ROLLBACK")

        # ============================================================
        # Case D — aborted xmin is never visible.
        # ============================================================
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (9, 'pad_9')")
        a.exec("ROLLBACK")
        t = Session(port)
        t.exec("BEGIN")
        assert_visible(t, 9, 0, "D", seed)
        t.exec("COMMIT")

        # ============================================================
        # Case E — xmin committed AFTER T's snapshot → invisible.
        # ============================================================
        t = Session(port)
        t.exec("BEGIN")
        a.exec("BEGIN")
        a.exec("INSERT INTO mvcc VALUES (10, 'pad_10')")
        a.exec("COMMIT")            # xmin > T.snapshot
        assert_visible(t, 10, 0, "E", seed)
        t.exec("COMMIT")
        # After T commits, a fresh session sees the row.
        t2 = Session(port)
        t2.exec("BEGIN")
        assert_visible(t2, 10, 1, "E-after", seed)
        t2.exec("COMMIT")
        t2.close()

        a.close()

        print(f"mvcc_visibility_matrix PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"mvcc_visibility_matrix FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
