#!/usr/bin/env python3
"""Persistent xid -> status log (ACID A1).

The in-memory TxnSlot table only holds the most-recent-N transactions.
Once a slot is recycled, the previous transaction's final state is
unavailable from the slot. For committed transactions that is harmless
(their writes survived). For aborted transactions, however, the lost
status risks treating a stale xmin as committed if rollback was ever
interrupted.

The new TxnStatusLog persists every commit / abort to
`<db>/wal/txn_status.log`. The visibility predicate consults it when a
slot has been recycled. This test verifies:

  1. The file is created on first commit/abort and contains one record
     per finished transaction.
  2. Records survive a clean restart.
  3. Aborted transaction ids stay queryable via the log even when their
     slot is long gone — exercised by running enough committed
     transactions to recycle the original slot table.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    minidb_query,
    run_minidb,
    temp_db,
)


def read_status_log(db: str) -> dict[int, int]:
    """Returns {xid: state} from txn_status.log (1=committed, 2=aborted)."""
    path = os.path.join(db, "wal", "txn_status.log")
    if not os.path.exists(path):
        return {}
    out: dict[int, int] = {}
    with open(path, "rb") as f:
        while True:
            buf = f.read(9)
            if len(buf) < 9:
                break
            xid, state = struct.unpack("<QB", buf)
            out[xid] = state
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-clog.")
    try:
        # ---- 1: a clean run produces one record per committed + aborted txn
        out = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT PRIMARY KEY);",
            "BEGIN;",
            "INSERT INTO t VALUES (1);",
            "COMMIT;",
            "BEGIN;",
            "INSERT INTO t VALUES (2);",
            "ROLLBACK;",
            "BEGIN;",
            "INSERT INTO t VALUES (3);",
            "COMMIT;",
        ])
        if "Error" in out:
            raise AssertionError(f"setup failed: {out}")

        log = read_status_log(db)
        committed = [x for x, s in log.items() if s == 1]
        aborted = [x for x, s in log.items() if s == 2]
        if len(committed) < 2:
            raise AssertionError(
                f"expected >=2 committed records in CLOG, got {committed}")
        if len(aborted) < 1:
            raise AssertionError(
                f"expected >=1 aborted record in CLOG, got {aborted}")

        # ---- 2: records persist across a clean restart
        rows = minidb_query(args.bin, db, "SELECT id FROM t ORDER BY id;", seed)
        if rows != [("1",), ("3",)]:
            raise AssertionError(
                f"committed rows missing after restart: {rows}")
        log2 = read_status_log(db)
        if len(log2) < len(log):
            raise AssertionError(
                f"CLOG shrank across restart: before={log}, after={log2}")

        # ---- 3: lots of new transactions recycle the slot table; aborted
        # xid from the original session is still recorded as aborted.
        churn = ["BEGIN;", "INSERT INTO t VALUES ({i});", "COMMIT;"]
        bigsql: list[str] = ["CREATE TABLE noise (id INT PRIMARY KEY);"]
        for i in range(4, 50):
            bigsql.append(f"BEGIN; INSERT INTO noise VALUES ({i}); COMMIT;")
        out3 = run_minidb(args.bin, db, bigsql)
        if "Error" in out3:
            raise AssertionError(f"churn failed: {out3}")

        log3 = read_status_log(db)
        # The original aborted xid must still report state=2 (aborted).
        for x in aborted:
            if log3.get(x) != 2:
                raise AssertionError(
                    f"aborted xid {x} lost across churn: log3[{x}]={log3.get(x)!r}")

        print(f"clog_status_log PASS seed={seed} "
              f"committed={len(committed)} aborted={len(aborted)} "
              f"after_churn={len(log3)}")
        return 0
    except Exception as exc:
        print(f"clog_status_log FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
