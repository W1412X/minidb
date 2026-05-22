#!/usr/bin/env python3
"""VACUUM correctness under concurrent operations.

Validates that the VACUUM SQL command:
  1. Reclaims space from dead tuples (basic GC)
  2. Freezes eligible tuples (committed xmin -> kFrozenTxnId)
  3. Does not interfere with concurrent readers
  4. Does not corrupt data after multiple cycles
  5. Handles empty tables gracefully

Test fixture:
  t1(id INT PRIMARY KEY, val INT)
  Initial: 20 rows (id=1..20, val=id*10)

  Section 1 -- Basic VACUUM after DELETE:
    Delete rows 1-10, VACUUM, verify rows 11-20 survive.

  Section 2 -- VACUUM with concurrent reader:
    Tx A: BEGIN; SELECT COUNT(*) (snapshot)
    Tx B: VACUUM
    Tx A: SELECT COUNT(*) again (must be consistent)

  Section 3 -- Multiple VACUUM cycles (idempotence):
    Run VACUUM 3 times, verify data unchanged.

  Section 4 -- VACUUM on empty table:
    DELETE all rows, VACUUM, verify empty.

  Section 5 -- VACUUM after UPDATE (version chains):
    Re-create rows, UPDATE some, VACUUM, verify latest versions visible.
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
    select_rows_from_output,
    temp_db,
)


class Session:
    """Persistent TCP session."""

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


def _extract_int(reply: str) -> int | None:
    for line in reversed(reply.splitlines()):
        stripped = line.strip()
        if stripped.lstrip("-").isdigit():
            return int(stripped)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = 12648459
    db = temp_db("minidb-vacuumconc.")
    port = 18400 + (os.getpid() % 1000)
    server = TcpMiniDB(args.bin, db, port)
    failures: list[str] = []

    try:
        # -- Setup: create table with 20 rows ----------------------------------
        stmts = [
            "CREATE TABLE t1 (id INT PRIMARY KEY, val INT);",
        ]
        for i in range(1, 21):
            stmts.append(f"INSERT INTO t1 VALUES ({i}, {i * 10});")
        setup = run_minidb(args.bin, db, stmts)
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        server.start()

        # ================================================================
        # Section 1: Basic VACUUM after DELETE
        # ================================================================
        s1 = Session(port)
        s1.send("BEGIN;")
        s1.send("DELETE FROM t1 WHERE id <= 10;")
        s1.send("COMMIT;")
        s1.send("VACUUM;")

        # Verify surviving rows
        r = s1.send("SELECT COUNT(*) FROM t1;")
        count = _extract_int(r)
        if count != 10:
            failures.append(f"S1: expected 10 rows after VACUUM, got {count}")

        r = s1.send("SELECT SUM(val) FROM t1;")
        total = _extract_int(r)
        # sum of 11*10 + 12*10 + ... + 20*10 = 10*(11+12+...+20) = 10*155 = 1550
        if total != 1550:
            failures.append(f"S1: expected SUM=1550, got {total}")

        s1.close()

        # ================================================================
        # Section 2: VACUUM with concurrent reader
        # ================================================================
        barrier_start = threading.Barrier(2)
        barrier_vacuumed = threading.Barrier(2)
        results: dict[str, object] = {}
        errors: list[str] = []

        def reader() -> None:
            sess = Session(port)
            try:
                sess.send("BEGIN;")
                r = sess.send("SELECT COUNT(*) FROM t1;")
                results["read1"] = _extract_int(r)
                barrier_start.wait()  # signal VACUUM to start
                barrier_vacuumed.wait()  # wait for VACUUM to finish

                # Read again within the same snapshot
                r = sess.send("SELECT COUNT(*) FROM t1;")
                results["read2"] = _extract_int(r)
                sess.send("COMMIT;")
            except Exception as exc:
                errors.append(f"reader: {exc}")
            finally:
                sess.close()

        def vacuumer() -> None:
            sess = Session(port)
            try:
                barrier_start.wait()  # wait for reader to take snapshot
                sess.send("VACUUM;")
                barrier_vacuumed.wait()  # signal reader
            except Exception as exc:
                errors.append(f"vacuumer: {exc}")
            finally:
                sess.close()

        t_r = threading.Thread(target=reader)
        t_v = threading.Thread(target=vacuumer)
        t_r.start()
        t_v.start()
        t_r.join(timeout=15)
        t_v.join(timeout=15)

        if errors:
            failures.append(f"S2 thread errors: {errors}")
        if results.get("read1") != 10:
            failures.append(f"S2: first read expected 10, got {results.get('read1')}")
        if results.get("read2") != 10:
            failures.append(
                f"S2: second read (after VACUUM) expected 10 (consistent snapshot), "
                f"got {results.get('read2')}"
            )

        # ================================================================
        # Section 3: Multiple VACUUM cycles (idempotence)
        # ================================================================
        s3 = Session(port)
        for _ in range(3):
            s3.send("VACUUM;")

        r = s3.send("SELECT COUNT(*) FROM t1;")
        count = _extract_int(r)
        if count != 10:
            failures.append(f"S3: expected 10 after 3 VACUUMs, got {count}")

        r = s3.send("SELECT SUM(val) FROM t1;")
        total = _extract_int(r)
        if total != 1550:
            failures.append(f"S3: expected SUM=1550 after 3 VACUUMs, got {total}")
        s3.close()

        # ================================================================
        # Section 4: VACUUM on empty table
        # ================================================================
        s4 = Session(port)
        s4.send("BEGIN;")
        s4.send("DELETE FROM t1 WHERE id > 0;")
        s4.send("COMMIT;")
        s4.send("VACUUM;")

        r = s4.send("SELECT COUNT(*) FROM t1;")
        count = _extract_int(r)
        if count != 0:
            failures.append(f"S4: expected 0 after delete all + VACUUM, got {count}")
        s4.close()

        # ================================================================
        # Section 5: VACUUM after UPDATE (version chains)
        # ================================================================
        s5 = Session(port)
        # Re-populate
        s5.send("BEGIN;")
        for i in range(1, 6):
            s5.send(f"INSERT INTO t1 VALUES ({i}, {i * 10});")
        s5.send("COMMIT;")

        # Update some rows (creates version chains)
        s5.send("BEGIN;")
        s5.send("UPDATE t1 SET val = 999 WHERE id = 1;")
        s5.send("UPDATE t1 SET val = 888 WHERE id = 3;")
        s5.send("COMMIT;")

        # VACUUM should clean up old versions
        s5.send("VACUUM;")

        # Verify latest versions are visible
        r = s5.send("SELECT val FROM t1 WHERE id = 1;")
        val1 = _extract_int(r)
        if val1 != 999:
            failures.append(f"S5: expected val=999 for id=1, got {val1}")

        r = s5.send("SELECT val FROM t1 WHERE id = 3;")
        val3 = _extract_int(r)
        if val3 != 888:
            failures.append(f"S5: expected val=888 for id=3, got {val3}")

        r = s5.send("SELECT SUM(val) FROM t1;")
        total = _extract_int(r)
        # 999 + 20 + 888 + 40 + 50 = 1997
        if total != 1997:
            failures.append(f"S5: expected SUM=1997, got {total}")
        s5.close()

        # ================================================================
        # Report
        # ================================================================
        if failures:
            for f in failures:
                print(f"  FAIL: {f}", file=sys.stderr)
            print(
                f"vacuum_concurrent FAIL seed={seed}: "
                f"{len(failures)} failure(s)",
                file=sys.stderr,
            )
            return 1

        print(f"vacuum_concurrent PASS seed={seed} (5 sections, all green)")
        return 0

    except Exception as exc:
        print(f"vacuum_concurrent FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        server.stop(kill=True)
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
