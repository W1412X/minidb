#!/usr/bin/env python3
"""Regression test: malformed SELECT statements must produce a diagnostic.

Before the fix, the parser's parse_primary() treated every keyword in
the KW_SELECT..KW_DEALLOCATE range as a valid column reference (via
is_identifier_token). So `SELECT FROM t;` parsed silently as
"SELECT [column-named-FROM] FROM t" and the executor returned an
empty result with no error line at all.

After the fix, parse_select_body() peeks the first token of the select
list and rejects any keyword that introduces a downstream SELECT
clause (FROM/WHERE/GROUP/HAVING/ORDER/LIMIT/OFFSET/UNION/JOIN/...)
with `expected select expression`. Same for `SELECT ;` and bare
`SELECT`.
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    cleanup,
    run_minidb,
    temp_db,
)


CASES = [
    "SELECT FROM t;",
    "SELECT WHERE a = 1;",
    "SELECT GROUP BY a;",
    "SELECT ORDER BY a;",
    "SELECT LIMIT 5;",
    "SELECT ;",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-sel-empty.")
    try:
        setup = run_minidb(args.bin, db, [
            "CREATE TABLE t (a INT);",
            "INSERT INTO t VALUES (1);",
        ])
        if "Error" in setup:
            raise AssertionError(f"setup failed: {setup}")

        for sql in CASES:
            out = run_minidb(args.bin, db, [sql])
            if "Error: expected select expression" not in out:
                raise AssertionError(
                    f"expected 'Error: expected select expression' for {sql!r} "
                    f"(seed={seed})\n{out}"
                )

        # Sanity: a well-formed SELECT must still work.
        ok = run_minidb(args.bin, db, ["SELECT a FROM t;"])
        if "Error" in ok or "\n1" not in ok:
            raise AssertionError(
                f"well-formed SELECT regressed (seed={seed})\n{ok}"
            )

        print(f"select_empty_list PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"select_empty_list FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
