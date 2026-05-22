#!/usr/bin/env python3
"""Snapshot Isolation prevents phantom reads within a transaction's snapshot.

A phantom read occurs when a transaction re-executes a range query and finds
new rows that were inserted by another concurrent transaction. Under Snapshot
Isolation (SI), each transaction reads from a consistent snapshot taken at
BEGIN time, so inserts committed by other transactions after that point are
invisible until the reading transaction starts a new one.

Test fixture:
  accounts(id INT PRIMARY KEY, category INT, balance INT)
  Initial rows: (1,1,100), (2,1,100), (3,1,100)  -- all category=1

  Phase 1 -- Phantom prevented within snapshot:
    Tx A (SI): BEGIN; SELECT SUM(balance) FROM accounts WHERE category = 1;
               -> sees 300
    Tx B (SI): INSERT INTO accounts VALUES (4, 1, 100); COMMIT;
    Tx A:      SELECT SUM(balance) FROM accounts WHERE category = 1;
               -> still sees 300 (snapshot isolation -- Tx B's insert invisible)
    Tx A:      COMMIT;

  Phase 2 -- Phantom visible after commit:
    New query: SELECT SUM(balance) FROM accounts WHERE category = 1;
               -> sees 400 (all 4 rows now visible)

This is correct SI behavior, not a bug. The test documents and pins the
guarantee that snapshot isolation provides repeatable range-query results
within a single transaction.
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


def _extract_sum(reply: str) -> int | None:
    """Extract the numeric SUM result from a minidb TCP reply.

    The reply typically contains a header line (e.g. "SUM(balance)") followed
    by the value line.  We grab the last purely-numeric line.
    """
    for line in reversed(reply.splitlines()):
        stripped = line.strip()
        if stripped.lstrip("-").isdigit():
            return int(stripped)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = 12648456
    db = temp_db("minidb-phantom.")
    port = 18200 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    try:
        # -- Setup: create table and insert 3 rows with category=1 ----------
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE accounts (id INT PRIMARY KEY, category INT, balance INT);",
            "INSERT INTO accounts VALUES (1, 1, 100);",
            "INSERT INTO accounts VALUES (2, 1, 100);",
            "INSERT INTO accounts VALUES (3, 1, 100);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()

        # ================================================================
        # Phase 1: Phantom prevented within a snapshot
        # ================================================================
        # Barriers coordinate the two transactions so that Tx B's INSERT
        # happens between Tx A's first and second SELECT.
        after_first_read = threading.Barrier(2)
        after_b_commit = threading.Barrier(2)

        results: dict[str, object] = {}
        errors: list[str] = []

        def tx_a() -> None:
            sess = Session(port)
            try:
                sess.send("BEGIN;")
                # First range query -- should see 3 rows, sum = 300
                r1 = sess.send(
                    "SELECT SUM(balance) FROM accounts WHERE category = 1;"
                )
                results["a_read1"] = r1
                after_first_read.wait()  # signal Tx B to insert
                after_b_commit.wait()    # wait for Tx B to commit

                # Second range query -- SI snapshot must still show 300
                r2 = sess.send(
                    "SELECT SUM(balance) FROM accounts WHERE category = 1;"
                )
                results["a_read2"] = r2
                results["a_commit"] = sess.send("COMMIT;")
            except Exception as exc:
                errors.append(f"tx_a: {exc}")
            finally:
                sess.close()

        def tx_b() -> None:
            sess = Session(port)
            try:
                after_first_read.wait()  # wait until Tx A has done first read
                sess.send("BEGIN;")
                ins = sess.send(
                    "INSERT INTO accounts VALUES (4, 1, 100);"
                )
                results["b_insert"] = ins
                results["b_commit"] = sess.send("COMMIT;")
                after_b_commit.wait()    # signal Tx A to continue
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

        # Validate Tx B committed successfully
        b_commit = str(results.get("b_commit", ""))
        if "Error" in b_commit:
            raise AssertionError(f"Tx B insert/commit failed: {b_commit}")

        # Validate Tx A's first read = 300
        sum1 = _extract_sum(str(results.get("a_read1", "")))
        if sum1 != 300:
            raise AssertionError(
                f"Tx A first read: expected SUM=300, got {sum1} "
                f"(raw: {results.get('a_read1')!r})"
            )

        # Validate Tx A's second read = 300 (phantom prevented by SI)
        sum2 = _extract_sum(str(results.get("a_read2", "")))
        if sum2 != 300:
            raise AssertionError(
                f"Tx A second read: expected SUM=300 (no phantom), got {sum2} "
                f"(raw: {results.get('a_read2')!r})"
            )

        # Validate Tx A committed successfully
        a_commit = str(results.get("a_commit", ""))
        if "Error" in a_commit:
            raise AssertionError(f"Tx A commit failed: {a_commit}")

        # ================================================================
        # Phase 2: Phantom visible after commit
        # ================================================================
        # A fresh query outside any prior snapshot must see all 4 rows.
        rows = minidb_query(
            args.bin, db,
            "SELECT SUM(balance) FROM accounts WHERE category = 1;",
            seed,
        )
        # minidb_query returns parsed rows; the SUM result is a single cell.
        if not rows:
            raise AssertionError("post-commit SUM query returned no rows")
        post_sum = int(rows[0][0])
        if post_sum != 400:
            raise AssertionError(
                f"post-commit SUM: expected 400 (4 rows visible), got {post_sum}"
            )

        print(
            f"phantom_read PASS seed={seed} "
            f"(SI prevented phantom within snapshot: "
            f"read1={sum1}, read2={sum2}, post_commit={post_sum})"
        )
        return 0
    except Exception as exc:
        print(f"phantom_read FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
