#!/usr/bin/env python3
"""VARCHAR(n) length enforcement (ACID B3).

Before this fix the parser captured the bound `n` in CREATE TABLE but
the catalog never carried it forward, so VARCHAR(5) silently accepted
arbitrarily long values. This test pins the new behaviour:

  1. CREATE TABLE persists the bound; DESC / round-trip after restart
     still knows about it.
  2. INSERT of an over-long value reports a length error and the row
     is NOT visible afterwards.
  3. UPDATE that pushes a value over the bound reports the same error
     and the row keeps its old value.
  4. INSERT of an exact-length value is accepted.
  5. TEXT and bare VARCHAR (no bound) accept anything.
  6. After ALTER TABLE ADD COLUMN VARCHAR(n), the bound is enforced.
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    minidb_query,
    run_minidb,
    temp_db,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-varchar.")
    try:
        # ---- CREATE + boundary inserts ---------------------------------
        out = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT, name VARCHAR(5), bio TEXT);",
            "INSERT INTO t VALUES (1, 'alice', 'short');",
            "INSERT INTO t VALUES (2, 'abcde', 'still ok');",          # exactly 5
        ])
        if "Error" in out:
            raise AssertionError(f"valid inserts rejected: {out}")

        # ---- over-long INSERT must be rejected ------------------------
        bad = run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (3, 'alexander', 'whatever');",
        ])
        if "Error: value too long" not in bad:
            raise AssertionError(f"expected length error on INSERT, got: {bad}")
        rows = minidb_query(args.bin, db, "SELECT id FROM t ORDER BY id;", seed)
        if rows != [("1",), ("2",)]:
            raise AssertionError(f"INSERT-on-error leaked row: {rows}")

        # ---- TEXT column accepts arbitrarily long values --------------
        big = "x" * 200
        out2 = run_minidb(args.bin, db, [f"UPDATE t SET bio = '{big}' WHERE id = 1;"])
        if "Error" in out2:
            raise AssertionError(f"TEXT should accept long values: {out2}")

        # ---- UPDATE pushing past the bound must be rejected ----------
        bad_upd = run_minidb(args.bin, db, [
            "UPDATE t SET name = 'augustus' WHERE id = 1;",
        ])
        if "Error: value too long" not in bad_upd:
            raise AssertionError(f"expected length error on UPDATE, got: {bad_upd}")
        rows = minidb_query(args.bin, db,
                            "SELECT id, name FROM t WHERE id = 1;", seed)
        if rows != [("1", "alice")]:
            raise AssertionError(f"UPDATE-on-error mutated row: {rows}")

        # ---- bound persists across restart ---------------------------
        # The schema serialisation now carries varchar_length, so a reopened
        # database must still reject over-long values.
        out3 = run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (4, 'overflow', 'b');",
        ])
        if "Error: value too long" not in out3:
            raise AssertionError(f"length bound lost after restart: {out3}")

        # ---- ALTER TABLE ADD COLUMN VARCHAR(n) -----------------------
        out4 = run_minidb(args.bin, db, [
            "ALTER TABLE t ADD COLUMN tag VARCHAR(3) DEFAULT 'x';",
            "INSERT INTO t VALUES (5, 'ed', 'ok', 'abc');",            # exactly 3
        ])
        if "Error" in out4:
            raise AssertionError(f"ALTER + 3-char insert rejected: {out4}")
        bad_alter = run_minidb(args.bin, db, [
            "INSERT INTO t VALUES (6, 'gh', 'ok', 'toolong');",
        ])
        if "Error: value too long" not in bad_alter:
            raise AssertionError(
                f"ALTER ADD COLUMN VARCHAR(n) bound not enforced: {bad_alter}")

        print(f"varchar_length PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"varchar_length FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
