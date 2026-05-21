#!/usr/bin/env python3
"""Torn-page protection test for ACID item D4.

Validates the two layers of torn-page protection:

  1. page_checksum: a corrupted data page is detected on read and is NOT
     surfaced as garbage rows.
  2. doublewrite: a synthesized doublewrite.bin restores the page on
     startup before any other code touches the file.

These are the two invariants the storage layer must hold so that a
power-loss in the middle of an 8 KB page write does not corrupt the
database.
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


PAGE_SIZE = 8192
CHECKSUM_SEED = 0xA5A5
DOUBLEWRITE_MAGIC = 0x4D44425752495445  # "MDBWRITE" little-endian per struct
DOUBLEWRITE_HEADER_FMT = "<QQHHI"        # magic, page_id, checksum, reserved, page_size
DOUBLEWRITE_HEADER_SIZE = struct.calcsize(DOUBLEWRITE_HEADER_FMT)


def page_checksum(data: bytes) -> int:
    """Mirror of DiskManager::page_checksum in disk_manager.cpp."""
    if len(data) != PAGE_SIZE:
        raise ValueError(f"expected {PAGE_SIZE}-byte page, got {len(data)}")
    s = CHECKSUM_SEED
    for i, b in enumerate(data):
        if i == 22 or i == 23:  # PageHeader::reserved holds the checksum
            continue
        s = (s + b) & 0xFFFF
        s = (((s << 1) | (s >> 15))) & 0xFFFF
    return 1 if s == 0 else s


def first_data_page(heap_path: str) -> tuple[int, bytes]:
    """Return (page_num, page_bytes) for the first non-zero data page."""
    with open(heap_path, "rb") as f:
        blob = f.read()
    if len(blob) % PAGE_SIZE != 0 or len(blob) < PAGE_SIZE * 2:
        raise AssertionError(f"unexpected heap size: {len(blob)}")
    # Page 0 is the file-meta page in MiniDB; the first real data page is 1.
    for page_num in range(1, len(blob) // PAGE_SIZE):
        page = blob[page_num * PAGE_SIZE:(page_num + 1) * PAGE_SIZE]
        if any(page):
            return page_num, page
    raise AssertionError("no non-zero data page found")


def write_at(path: str, offset: int, payload: bytes) -> None:
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(payload)


def synthesize_doublewrite(path: str, page_id: int, page: bytes) -> None:
    """Write a valid doublewrite.bin recording (page_id, page)."""
    chksum = page_checksum(page)
    header = struct.pack(DOUBLEWRITE_HEADER_FMT, DOUBLEWRITE_MAGIC, page_id, chksum, 0, PAGE_SIZE)
    with open(path, "wb") as f:
        f.write(header)
        f.write(page)


def heap_file_for_table(db_dir: str) -> str:
    tables_dir = os.path.join(db_dir, "tables")
    heaps = [f for f in os.listdir(tables_dir) if f.endswith(".heap")]
    if len(heaps) != 1:
        raise AssertionError(f"expected exactly one heap file, got {heaps}")
    return os.path.join(tables_dir, heaps[0])


def truncate_wal(db_dir: str) -> None:
    """Empty the WAL so the next startup does not redo writes.

    The torn-page test wants to exercise checksum/doublewrite paths in
    isolation. Without this step, WAL replay would simply re-insert the
    tuples and mask any failure in the lower storage layer.
    """
    wal_path = os.path.join(db_dir, "wal", "wal.log")
    if os.path.exists(wal_path):
        open(wal_path, "w").close()


def run_phase_checksum_protection(bin_path: str, seed: int) -> None:
    """Corrupt a page on disk. After restart the row must NOT appear as garbage."""
    db = temp_db("minidb-torn-checksum.")
    try:
        out = run_minidb(bin_path, db, [
            "CREATE TABLE torn (id INT PRIMARY KEY, v TEXT);",
            "INSERT INTO torn VALUES (1, 'alice');",
        ])
        if "Error" in out:
            raise AssertionError(f"setup failed: {out}")
        heap = heap_file_for_table(db)
        page_num, _ = first_data_page(heap)

        # Flip 128 bytes near the middle of the page (away from the checksum
        # slot at offset 22..23) to invalidate the stored checksum.
        offset = page_num * PAGE_SIZE + PAGE_SIZE // 2
        write_at(heap, offset, b"\xDE\xAD\xBE\xEF" * 32)
        # Drop the WAL so this phase exercises checksum protection alone,
        # not WAL replay re-inserting the tuple onto a freshly-zeroed page.
        truncate_wal(db)

        # No doublewrite.bin was prepared, so recover_double_write is a no-op.
        # The torn page must be read back as detected-corrupt (zeroed), never
        # as garbage tuples.
        rows = minidb_query(bin_path, db, "SELECT id, v FROM torn ORDER BY id;", seed)
        if rows == [("1", "alice")]:
            raise AssertionError(
                "checksum protection failed: corrupted page returned the "
                "pre-corruption row even though the on-disk bytes were "
                "overwritten with garbage."
            )
        for row in rows:
            if row[0] != "1" or row[1] != "alice":
                raise AssertionError(f"garbage tuple surfaced from corrupted page: {row}")
    finally:
        cleanup(db)


def run_phase_doublewrite_restore(bin_path: str, seed: int) -> None:
    """Synthesize doublewrite.bin and confirm recovery uses it to repair a torn page."""
    db = temp_db("minidb-torn-doublewrite.")
    try:
        out = run_minidb(bin_path, db, [
            "CREATE TABLE torn (id INT PRIMARY KEY, v TEXT);",
            "INSERT INTO torn VALUES (1, 'alice');",
            "INSERT INTO torn VALUES (2, 'bob');",
        ])
        if "Error" in out:
            raise AssertionError(f"setup failed: {out}")

        heap_path = heap_file_for_table(db)
        page_num, good_page = first_data_page(heap_path)
        file_id = int(os.path.basename(heap_path).split(".")[0])
        page_id = (file_id << 32) | page_num

        # Tear the on-disk page so the next read would fail the checksum check.
        torn_payload = b"\x00" * (PAGE_SIZE // 2)
        write_at(heap_path, page_num * PAGE_SIZE, torn_payload)

        # Place a valid doublewrite record holding the pre-torn copy. On open,
        # DiskManager::recover_double_write must restore it before any other
        # code reads the page.
        synthesize_doublewrite(os.path.join(db, "doublewrite.bin"),
                               page_id, good_page)
        # Drop the WAL so the restore must come from doublewrite.bin, not
        # from redoing the original INSERT records.
        truncate_wal(db)

        rows = minidb_query(bin_path, db, "SELECT id, v FROM torn ORDER BY id;", seed)
        if rows != [("1", "alice"), ("2", "bob")]:
            raise AssertionError(
                f"doublewrite restore did not recover the page. Got rows={rows}"
            )
    finally:
        cleanup(db)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    try:
        run_phase_checksum_protection(args.bin, seed)
        run_phase_doublewrite_restore(args.bin, seed)
        print(f"torn_page PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"torn_page FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
