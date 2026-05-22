#!/usr/bin/env python3
"""Long-running snapshot prevents GC from collecting visible tuples.

MVCC garbage collection must never reclaim a tuple version that is still
visible to any active transaction.  This test verifies that a long-running
snapshot (Tx A) can still see rows that were deleted and GC'd by another
transaction (Tx B + VACUUM), because the is_garbage() check must respect
the oldest active transaction ID.

Test fixture:
  items(id INT PRIMARY KEY, val INT)
  Initial rows: (1,10), (2,20), (3,30), (4,40), (5,50)

  Phase 1 -- Long snapshot survives GC:
    Tx A: BEGIN (takes snapshot)
    Tx A: SELECT SUM(val) FROM items -> 150
    Tx B: DELETE FROM items WHERE id <= 3; COMMIT;
    Tx B: VACUUM;  (triggers full GC + freeze)
    Tx A: SELECT SUM(val) FROM items -> still 150 (snapshot isolation)
    Tx A: COMMIT

  Phase 2 -- After Tx A finishes, GC can reclaim:
    New query: SELECT SUM(val) FROM items -> 90  (only rows 4,5 remain)
    VACUUM again: should successfully reclaim the deleted rows

  Phase 3 -- Freeze verification:
    After VACUUM, surviving rows (4,5) should have been frozen.
    A fresh query should still see them (frozen xmin visibility fast-path).
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
    """Persistent TCP session -- keeps BEGIN..COMMIT on one connection."""

    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(2)
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
                if buf.endswith(b"\n"):
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


def _extract_sum(reply: str) -> int | None:
    for line in reversed(reply.splitlines()):
        stripped = line.strip()
        if stripped.lstrip("-").isdigit():
            return int(stripped)
    return None


def _extract_count(reply: str) -> int | None:
    for line in reversed(reply.splitlines()):
        stripped = line.strip()
        if stripped.isdigit():
            return int(stripped)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = 12648458
    db = temp_db("minidb-longgc.")
    port = 18300 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    try:
        # -- Setup -------------------------------------------------------------
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE items (id INT PRIMARY KEY, val INT);",
            "INSERT INTO items VALUES (1, 10);",
            "INSERT INTO items VALUES (2, 20);",
            "INSERT INTO items VALUES (3, 30);",
            "INSERT INTO items VALUES (4, 40);",
            "INSERT INTO items VALUES (5, 50);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()

        # ================================================================
        # Phase 1: Long snapshot survives GC
        # ================================================================
        after_a_read = threading.Barrier(2)
        after_vacuum = threading.Barrier(2)

        results: dict[str, object] = {}
        errors: list[str] = []

        def tx_a() -> None:
            sess = Session(port)
            try:
                sess.send("BEGIN;")
                # First read -- snapshot sees all 5 rows
                r1 = sess.send("SELECT SUM(val) FROM items;")
                results["a_sum1"] = _extract_sum(r1)
                after_a_read.wait()  # signal Tx B to delete + vacuum
                after_vacuum.wait()  # wait for VACUUM to finish

                # Second read -- snapshot must still see 150
                r2 = sess.send("SELECT SUM(val) FROM items;")
                results["a_sum2"] = _extract_sum(r2)

                # Also check count
                r3 = sess.send("SELECT COUNT(*) FROM items;")
                results["a_count"] = _extract_count(r3)

                sess.send("COMMIT;")
            except Exception as exc:
                errors.append(f"tx_a: {exc}")
            finally:
                sess.close()

        def tx_b() -> None:
            sess = Session(port)
            try:
                after_a_read.wait()  # wait for Tx A to take snapshot
                # Delete first 3 rows and commit
                sess.send("BEGIN;")
                sess.send("DELETE FROM items WHERE id <= 3;")
                sess.send("COMMIT;")
                # Run VACUUM to trigger GC + freeze
                sess.send("VACUUM;")
                after_vacuum.wait()  # signal Tx A to continue
            except Exception as exc:
                errors.append(f"tx_b: {exc}")
            finally:
                sess.close()

        t_a = threading.Thread(target=tx_a)
        t_b = threading.Thread(target=tx_b)
        t_a.start()
        t_b.start()
        t_a.join(timeout=30)
        t_b.join(timeout=30)

        if errors:
            raise AssertionError(f"thread errors: {errors}")

        # Validate Tx A's first read
        if results.get("a_sum1") != 150:
            raise AssertionError(
                f"Phase 1 first read: expected SUM=150, got {results.get('a_sum1')}"
            )

        # Validate Tx A's second read (must still see 150 despite VACUUM)
        if results.get("a_sum2") != 150:
            raise AssertionError(
                f"Phase 1 second read: expected SUM=150 (GC must not collect "
                f"tuples visible to active snapshot), got {results.get('a_sum2')}"
            )

        # Validate count
        if results.get("a_count") != 5:
            raise AssertionError(
                f"Phase 1 count: expected 5, got {results.get('a_count')}"
            )

        # ================================================================
        # Phase 2: After long snapshot finishes, deleted rows are gone
        # ================================================================
        rows = minidb_query(
            args.bin, db,
            "SELECT SUM(val) FROM items;",
            seed,
        )
        if not rows:
            raise AssertionError("Phase 2: no rows returned")
        post_sum = int(rows[0][0])
        if post_sum != 90:
            raise AssertionError(
                f"Phase 2: expected SUM=90 (only rows 4,5), got {post_sum}"
            )

        # Run VACUUM again -- now GC can reclaim deleted rows
        run_minidb(args.bin, db, ["VACUUM;"])

        # Verify rows still correct after second VACUUM
        rows = minidb_query(
            args.bin, db,
            "SELECT COUNT(*) FROM items;",
            seed,
        )
        if not rows:
            raise AssertionError("Phase 2: count query returned no rows")
        count = int(rows[0][0])
        if count != 2:
            raise AssertionError(
                f"Phase 2: expected COUNT=2, got {count}"
            )

        # ================================================================
        # Phase 3: Frozen tuples are still visible
        # ================================================================
        # After VACUUM, surviving tuples should have been frozen.
        # Verify they're still accessible.
        rows = minidb_query(
            args.bin, db,
            "SELECT id, val FROM items ORDER BY id;",
            seed,
        )
        expected = [("4", "40"), ("5", "50")]
        if rows != expected:
            raise AssertionError(
                f"Phase 3 frozen visibility: expected {expected}, got {rows}"
            )

        print(
            f"long_snapshot_gc PASS seed={seed} "
            f"(snapshot survived GC: sum1={results['a_sum1']}, "
            f"sum2={results['a_sum2']}, post_commit_sum={post_sum})"
        )
        return 0
    except Exception as exc:
        print(f"long_snapshot_gc FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
