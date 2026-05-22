#!/usr/bin/env python3
"""WAL per-record corruption detection (ACID D1).

Every WAL record now carries a 32-bit magic prefix and a CRC32 over
the header + payload. A torn write or a wild byte flip anywhere in the
record body must:

  - cause replay to stop cleanly at the corrupted record (not crash,
    not loop, not re-interpret garbage as records),
  - leave the database in a consistent state up to the LAST verified
    record before the corruption, and
  - allow earlier committed work to remain visible after restart.

This test:

  1. Sets up a table and INSERTs three committed rows. To make sure
     WAL records survive the next restart, we DO NOT call CHECKPOINT
     between INSERTs; we kill the server with `subprocess.kill()` so
     no clean-shutdown checkpoint truncates the log.
  2. Pokes a single byte inside the SECOND record's body to invalidate
     its CRC.
  3. Reopens minidb and runs a read-only query.
  4. Asserts: the first INSERT's row is visible (replay reached it),
     the second/third rows are NOT visible (replay stopped at the
     corrupted record), and no error / crash escaped to the user.

The "stopped early" semantic is exactly what we want from a corrupted
log: don't make things worse by applying half-records.
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
    minidb_query,
    temp_db,
)


# Mirrors src/recovery/wal.h — must stay in sync if WalRecord ever changes.
WAL_RECORD_MAGIC = 0xD8BA110C
# struct WalRecord with default alignment:
#   u32 magic; u32 crc; u64 lsn; u64 txn_id; u16 type; u32 data_len;
# = 4 + 4 + 8 + 8 + 2 + 2(pad) + 4 = 32 bytes
WAL_HEADER_SIZE = 32


def write_clean_setup(bin_path: str, db_dir: str) -> None:
    """Insert three committed rows and SIGKILL — leaves WAL intact."""
    proc = subprocess.Popen(
        [bin_path, "--dir", db_dir],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    assert proc.stdin is not None
    proc.stdin.write("CREATE TABLE wal_t (id INT PRIMARY KEY, v INT);\n")
    proc.stdin.write("BEGIN;\nINSERT INTO wal_t VALUES (1, 100);\nCOMMIT;\n")
    proc.stdin.write("BEGIN;\nINSERT INTO wal_t VALUES (2, 200);\nCOMMIT;\n")
    proc.stdin.write("BEGIN;\nINSERT INTO wal_t VALUES (3, 300);\nCOMMIT;\n")
    proc.stdin.flush()
    time.sleep(0.4)
    proc.kill()
    proc.wait(timeout=5)


def parse_wal_records(path: str) -> list[tuple[int, int]]:
    """Return [(start_offset, total_size), ...] for every well-formed record."""
    with open(path, "rb") as f:
        blob = f.read()
    out: list[tuple[int, int]] = []
    off = 0
    while off + WAL_HEADER_SIZE <= len(blob):
        magic, crc, lsn, txn_id, type_, pad16, data_len = struct.unpack_from(
            "<IIQQHHI", blob, off
        )
        if magic != WAL_RECORD_MAGIC:
            break
        size = WAL_HEADER_SIZE + data_len
        if off + size > len(blob):
            break
        out.append((off, size))
        off += size
    return out


def corrupt_byte_in_record(path: str, record_offset: int, record_size: int) -> None:
    """Flip a single byte inside the record header so its CRC fails."""
    # Aim at the type field (offset 24..25 in the header). It's well inside
    # the CRC-protected region but not the magic, so the record still LOOKS
    # like a record at the header level until CRC verification runs.
    flip_at = record_offset + 24
    assert flip_at < record_offset + record_size
    with open(path, "r+b") as f:
        f.seek(flip_at)
        b = f.read(1)
        f.seek(flip_at)
        f.write(bytes([b[0] ^ 0xFF]))


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-wal-corrupt.")
    try:
        write_clean_setup(args.bin, db)
        wal = os.path.join(db, "wal", "wal.log")
        if not os.path.exists(wal):
            raise AssertionError("WAL file missing after setup")
        records = parse_wal_records(wal)
        # Records: kTxnBegin, kInsert, kTxnCommit per row + CREATE-TABLE-side
        # metadata. We want to corrupt something AFTER the first row is fully
        # committed so partial recovery yields exactly one visible row.
        if len(records) < 6:
            raise AssertionError(
                f"expected at least 6 WAL records, parsed {len(records)}"
            )

        # The first three records cover CREATE TABLE side effects plus
        # the BEGIN of the first INSERT. To leave row 1 fully recoverable
        # we corrupt the record right after row 1's COMMIT — that is the
        # BEGIN of the second transaction (records[4] in the typical run).
        # Search for the first kTxnBegin (type=1) whose preceding kTxnCommit
        # has already been seen — guarantees row 1 stayed clean.
        target_off, target_size = None, None
        seen_commit = 0
        for off, size in records:
            with open(wal, "rb") as f:
                f.seek(off + 24)   # type field
                t = struct.unpack("<H", f.read(2))[0]
            if t == 2:                 # kTxnCommit
                seen_commit += 1
                if seen_commit == 1:
                    continue
            if t == 1 and seen_commit >= 1:
                target_off, target_size = off, size
                break
        if target_off is None:
            raise AssertionError("could not find a target record to corrupt")

        corrupt_byte_in_record(wal, target_off, target_size)

        rows = minidb_query(args.bin, db,
                            "SELECT id FROM wal_t ORDER BY id;",
                            seed, timeout=15)
        ids = {r[0] for r in rows}
        if "1" not in ids:
            raise AssertionError(
                f"first row missing — recovery stopped too early. rows={rows}"
            )
        if "2" in ids or "3" in ids:
            raise AssertionError(
                f"recovery applied records past the corruption point. rows={rows}"
            )

        print(f"wal_record_corruption PASS seed={seed} ids={sorted(ids)}")
        return 0
    except Exception as exc:
        print(f"wal_record_corruption FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
