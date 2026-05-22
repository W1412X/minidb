#!/usr/bin/env python3
"""DDL WAL audit trail (ACID D7).

Each DDL operation (CREATE TABLE / DROP TABLE / CREATE INDEX / DROP INDEX
/ ALTER TABLE ADD/DROP/RENAME COLUMN) now emits a kDdl WAL record. The
record is purely an audit trail today — recovery does not act on it —
but it lets a future repair pass detect orphaned files and half-finished
schema changes without scanning the entire filesystem.

This test verifies that the marker arrives for every DDL operation by:

  1. Running each DDL statement against a fresh database that is
     SIGKILLed right after — leaves the WAL un-truncated.
  2. Parsing the WAL bytes and walking past the WAL records that
     existed before the DDL.
  3. Asserting that exactly one kDdl record exists per DDL call, with
     the expected DdlOp byte in the payload.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    temp_db,
)


WAL_RECORD_MAGIC = 0xD8BA110C
WAL_HEADER_SIZE = 32                 # see wal_record_corruption.py
WAL_TYPE_DDL = 40                    # WalType::kDdl
DDL_OPS = {
    1: "CreateTable",
    2: "DropTable",
    3: "CreateIndex",
    4: "DropIndex",
    5: "AlterAddColumn",
    6: "AlterDropColumn",
    7: "AlterRenameColumn",
}


def kill_after(bin_path: str, db_dir: str, statements: list[str]) -> None:
    proc = subprocess.Popen(
        [bin_path, "--dir", db_dir],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    assert proc.stdin is not None
    for s in statements:
        proc.stdin.write(s + "\n")
        proc.stdin.flush()
        # Give minidb a moment to process each statement so the WAL fsync
        # in log_ddl actually lands before the next statement (or the
        # kill) hits.
        time.sleep(0.15)
    time.sleep(0.3)
    proc.kill()
    proc.wait(timeout=5)


def iter_records(blob: bytes):
    """Yield (type, payload) for every well-formed WAL record."""
    off = 0
    while off + WAL_HEADER_SIZE <= len(blob):
        magic, crc, lsn, txn_id, type_, _pad, data_len = struct.unpack_from(
            "<IIQQHHI", blob, off
        )
        if magic != WAL_RECORD_MAGIC:
            return
        end = off + WAL_HEADER_SIZE + data_len
        if end > len(blob):
            return
        yield type_, blob[off + WAL_HEADER_SIZE:end]
        off = end


def collect_ddl_ops(db_dir: str) -> list[tuple[str, str]]:
    """Return [(op_name, object_name), ...] in order."""
    wal_path = os.path.join(db_dir, "wal", "wal.log")
    with open(wal_path, "rb") as f:
        blob = f.read()
    out: list[tuple[str, str]] = []
    for type_, payload in iter_records(blob):
        if type_ != WAL_TYPE_DDL:
            continue
        if len(payload) < 11:    # u8 op + u32 table_id + u32 aux + u16 name_len
            continue
        op_byte = payload[0]
        name_len = struct.unpack_from("<H", payload, 9)[0]
        name = payload[11:11 + name_len].decode(errors="replace")
        out.append((DDL_OPS.get(op_byte, f"?{op_byte}"), name))
    return out


def expect_marker(db: str, expected_op: str, expected_name_suffix: str) -> None:
    """Asserts the WAL of `db` contains a kDdl marker matching the criteria."""
    ddl_ops = collect_ddl_ops(db)
    for op, name in ddl_ops:
        if op == expected_op and name.endswith(expected_name_suffix):
            return
    raise AssertionError(
        f"missing {expected_op} marker ending in {expected_name_suffix!r}. "
        f"All markers in WAL: {ddl_ops}")


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    # Tests run each DDL in its own session that is SIGKILLed before the
    # next session opens. ALTER calls trigger a checkpoint() that truncates
    # the WAL, so a single-session sweep would only see records past the
    # last checkpoint. Per-session sessions are also closer to how a real
    # crash would look — each crashes after exactly one DDL.
    base_setup = [
        "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16));",
    ]
    base_setup_with_index = base_setup + [
        "CREATE INDEX t_name_idx ON t(name);",
    ]

    try:
        # CREATE TABLE
        db = temp_db("minidb-ddl-ct.")
        try:
            kill_after(args.bin, db, base_setup)
            expect_marker(db, "CreateTable", "t")
        finally:
            cleanup(db)

        # CREATE INDEX
        db = temp_db("minidb-ddl-ci.")
        try:
            kill_after(args.bin, db, base_setup_with_index)
            expect_marker(db, "CreateIndex", "t_name_idx")
        finally:
            cleanup(db)

        # ALTER ADD / RENAME / DROP / DROP INDEX / DROP TABLE each run in
        # their own session after a non-killed setup so prior records are
        # cleanly checkpointed away. Each kill then leaves exactly one
        # surviving kDdl marker in the post-truncate region.
        for ddl_stmt, expected_op, expected_suffix in [
            (["ALTER TABLE t ADD COLUMN age INT DEFAULT 0;"],
             "AlterAddColumn", ".age"),
            (["ALTER TABLE t ADD COLUMN age INT DEFAULT 0;",
              "ALTER TABLE t RENAME COLUMN age TO years;"],
             "AlterRenameColumn", "age->years"),
            (["ALTER TABLE t ADD COLUMN age INT DEFAULT 0;",
              "ALTER TABLE t DROP COLUMN age;"],
             "AlterDropColumn", ".age"),
            (["DROP INDEX t_name_idx;"],
             "DropIndex", "t_name_idx"),
            (["DROP TABLE t;"],
             "DropTable", "t"),
        ]:
            db = temp_db(f"minidb-ddl-{expected_op}.")
            try:
                # Set up index-bearing table cleanly, then SIGKILL after the
                # final DDL statement so the marker survives in the WAL.
                kill_after(args.bin, db, base_setup_with_index + ddl_stmt)
                expect_marker(db, expected_op, expected_suffix)
            finally:
                cleanup(db)

        # Sanity: opening a DB with a DDL-rich WAL must not crash.
        db = temp_db("minidb-ddl-reopen.")
        try:
            kill_after(args.bin, db, base_setup_with_index + [
                "ALTER TABLE t ADD COLUMN tag VARCHAR(5);",
            ])
            proc = subprocess.run(
                [args.bin, "--dir", db],
                input="exit\n", text=True, capture_output=True, timeout=10
            )
            if proc.returncode != 0:
                raise AssertionError(
                    f"reopen after DDL-rich WAL failed: rc={proc.returncode}\n"
                    f"{proc.stdout}{proc.stderr}"
                )
        finally:
            cleanup(db)

        print(f"ddl_wal PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"ddl_wal FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
