#!/usr/bin/env python3
"""SQL three-valued logic correctness (ACID C).

The relational model has three truth values (TRUE / FALSE / UNKNOWN)
because NULL means "the value is not known". Every site that consumes a
boolean — WHERE, HAVING, JOIN ON, CASE WHEN — must treat UNKNOWN as
"reject the row", and every operator that consumes NULL input must
propagate UNKNOWN forward. Aggregates have their own NULL-skip rules.
This test pins MiniDB's behaviour against the SQL standard / PostgreSQL.

Pinned behaviour:
  1. `WHERE x = NULL`, `WHERE x <> NULL` — both return zero rows.
  2. `WHERE x IS NULL` and `WHERE x IS NOT NULL` partition the table.
  3. Boolean 3VL: NULL OR TRUE → TRUE, NULL AND FALSE → FALSE,
     NULL OR NULL → NULL (filter rejects the row).
  4. `NOT IN (..., NULL, ...)` evaluates to UNKNOWN — no row matches.
  5. Aggregates: COUNT(*) counts NULLs, COUNT(col) skips NULLs,
     SUM / AVG / MIN / MAX skip NULL inputs. SUM/AVG on an all-NULL
     column return NULL; COUNT returns 0.
  6. DISTINCT treats all NULLs as one value.
  7. ORDER BY: NULLs sort consistently (MiniDB places them last in ASC).
  8. UNIQUE constraint: multiple NULL rows are allowed.
  9. LEFT JOIN with a NULL join key produces a NULL-filled right side.
 10. Arithmetic on NULL → NULL.
 11. CASE WHEN x IS NULL THEN ... — NULL is detected.
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


def assert_eq(label, got, expected):
    if got != expected:
        raise AssertionError(f"{label}: got {got!r}, expected {expected!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-null-3vl.")
    try:
        out = run_minidb(args.bin, db, [
            "CREATE TABLE t (id INT, v INT, s VARCHAR(20));",
            "INSERT INTO t VALUES (1, 10, 'a');",
            "INSERT INTO t VALUES (2, NULL, 'b');",
            "INSERT INTO t VALUES (3, 30, NULL);",
            "INSERT INTO t VALUES (4, NULL, NULL);",
            "INSERT INTO t VALUES (5, 10, 'a');",
        ])
        if "Error" in out:
            raise AssertionError(f"setup failed: {out}")

        # (1) `= NULL` and `<> NULL` always return UNKNOWN -> zero rows.
        rows = minidb_query(args.bin, db, "SELECT id FROM t WHERE v = NULL;", seed)
        assert_eq("WHERE v = NULL", rows, [])
        rows = minidb_query(args.bin, db, "SELECT id FROM t WHERE v <> NULL;", seed)
        assert_eq("WHERE v <> NULL", rows, [])

        # (2) IS NULL / IS NOT NULL partition the table on v.
        rows = minidb_query(args.bin, db, "SELECT id FROM t WHERE v IS NULL ORDER BY id;", seed)
        assert_eq("WHERE v IS NULL", rows, [("2",), ("4",)])
        rows = minidb_query(args.bin, db, "SELECT id FROM t WHERE v IS NOT NULL ORDER BY id;", seed)
        assert_eq("WHERE v IS NOT NULL", rows, [("1",), ("3",), ("5",)])

        # (3) Boolean 3VL.
        # NULL OR TRUE -> TRUE: WHERE v=10 OR s IS NULL should hit id 1,3,4,5.
        rows = minidb_query(args.bin, db,
                            "SELECT id FROM t WHERE v = 10 OR s IS NULL ORDER BY id;", seed)
        assert_eq("3VL OR", rows, [("1",), ("3",), ("4",), ("5",)])
        # NULL AND FALSE -> FALSE: WHERE v=10 AND s='nope' -> empty.
        rows = minidb_query(args.bin, db,
                            "SELECT id FROM t WHERE v = 10 AND s = 'nope';", seed)
        assert_eq("3VL AND with false", rows, [])

        # (4) NOT IN (..., NULL, ...) -> UNKNOWN -> no rows.
        rows = minidb_query(args.bin, db,
                            "SELECT id FROM t WHERE v NOT IN (10, NULL) ORDER BY id;", seed)
        assert_eq("NOT IN with NULL list", rows, [])
        # IN (10, NULL) -> matches v=10 rows; NULL row stays NULL (rejected).
        rows = minidb_query(args.bin, db,
                            "SELECT id FROM t WHERE v IN (10, NULL) ORDER BY id;", seed)
        assert_eq("IN with NULL list", rows, [("1",), ("5",)])

        # (5) Aggregates.
        rows = minidb_query(args.bin, db, "SELECT COUNT(*) FROM t;", seed)
        assert_eq("COUNT(*)", rows, [("5",)])
        rows = minidb_query(args.bin, db, "SELECT COUNT(v) FROM t;", seed)
        assert_eq("COUNT(v) skips NULL", rows, [("3",)])
        rows = minidb_query(args.bin, db, "SELECT SUM(v) FROM t;", seed)
        assert_eq("SUM skips NULL", rows, [("50",)])
        rows = minidb_query(args.bin, db, "SELECT MIN(v), MAX(v) FROM t;", seed)
        assert_eq("MIN/MAX skip NULL", rows, [("10", "30")])
        # AVG over {10, 30, 10} = 50/3.
        rows = minidb_query(args.bin, db, "SELECT AVG(v) FROM t;", seed)
        # The output is a float; accept the canonical PostgreSQL-ish form.
        if not rows or not rows[0]:
            raise AssertionError(f"AVG failed: {rows}")
        avg = float(rows[0][0])
        if abs(avg - (50.0 / 3.0)) > 1e-6:
            raise AssertionError(f"AVG wrong: {avg}")

        # All-NULL column: SUM/AVG/MIN/MAX -> NULL, COUNT -> 0.
        out = run_minidb(args.bin, db, [
            "CREATE TABLE allnull (v INT);",
            "INSERT INTO allnull VALUES (NULL);",
            "INSERT INTO allnull VALUES (NULL);",
        ])
        if "Error" in out:
            raise AssertionError(f"allnull setup: {out}")
        rows = minidb_query(args.bin, db, "SELECT COUNT(v), SUM(v) FROM allnull;", seed)
        assert_eq("COUNT/SUM over all-NULL", rows, [("0", "NULL")])

        # (6) DISTINCT collapses NULLs.
        rows = minidb_query(args.bin, db, "SELECT DISTINCT v FROM t ORDER BY v;", seed)
        # Distinct values: 10, 30, NULL. NULL sorts last under MiniDB's ordering.
        assert_eq("DISTINCT with NULL", rows, [("10",), ("30",), ("NULL",)])

        # (7) ORDER BY ASC -> NULLs last (MiniDB type-id ordering).
        rows = minidb_query(args.bin, db, "SELECT id, v FROM t ORDER BY v, id;", seed)
        # v=10 (id 1,5), v=30 (id 3), then NULLs (id 2,4).
        assert_eq("ORDER BY ASC NULL last", rows, [
            ("1", "10"), ("5", "10"), ("3", "30"), ("2", "NULL"), ("4", "NULL"),
        ])

        # (8) UNIQUE constraint allows multiple NULLs.
        out = run_minidb(args.bin, db, [
            "CREATE TABLE u (id INT, k INT UNIQUE);",
            "INSERT INTO u VALUES (1, NULL);",
            "INSERT INTO u VALUES (2, NULL);",
            "INSERT INTO u VALUES (3, 7);",
        ])
        if "Error" in out:
            raise AssertionError(f"UNIQUE-with-NULL setup rejected: {out}")
        rows = minidb_query(args.bin, db, "SELECT COUNT(*) FROM u;", seed)
        assert_eq("UNIQUE allows multiple NULLs", rows, [("3",)])
        # A duplicate non-NULL must still be rejected (silent skip — 0 rows).
        run_minidb(args.bin, db, ["INSERT INTO u VALUES (4, 7);"])
        rows = minidb_query(args.bin, db, "SELECT COUNT(*) FROM u WHERE k = 7;", seed)
        assert_eq("UNIQUE still rejects dup non-NULL", rows, [("1",)])

        # (9) LEFT JOIN on NULL key.
        # No row in t has v matching an integer NULL on the right; NULL row
        # in t.v must still appear in LEFT JOIN result with NULL right side.
        out = run_minidb(args.bin, db, [
            "CREATE TABLE r (k INT, label VARCHAR(10));",
            "INSERT INTO r VALUES (10, 'ten');",
            "INSERT INTO r VALUES (30, 'thirty');",
        ])
        if "Error" in out:
            raise AssertionError(f"LEFT JOIN setup: {out}")
        rows = minidb_query(args.bin, db,
            "SELECT t.id, r.label FROM t LEFT JOIN r ON t.v = r.k ORDER BY t.id;", seed)
        assert_eq("LEFT JOIN with NULL key", rows, [
            ("1", "ten"), ("2", "NULL"), ("3", "thirty"), ("4", "NULL"), ("5", "ten"),
        ])

        # (10) Arithmetic with NULL.
        rows = minidb_query(args.bin, db,
            "SELECT id, v + 1 FROM t WHERE v IS NULL ORDER BY id;", seed)
        assert_eq("NULL + 1 -> NULL", rows, [("2", "NULL"), ("4", "NULL")])

        # (11) CASE WHEN x IS NULL.
        rows = minidb_query(args.bin, db,
            "SELECT id, CASE WHEN v IS NULL THEN -1 ELSE v END FROM t ORDER BY id;", seed)
        assert_eq("CASE WHEN IS NULL", rows, [
            ("1", "10"), ("2", "-1"), ("3", "30"), ("4", "-1"), ("5", "10"),
        ])

        print(f"null_three_valued_logic PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"null_three_valued_logic FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
