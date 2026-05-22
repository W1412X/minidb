#!/usr/bin/env python3
"""P1 feature tests for MiniDB.

Covers:
  1. NULLS FIRST / NULLS LAST in ORDER BY
  2. CAST expression (INT, TEXT, FLOAT, DOUBLE, BOOL, NULL, invalid, in clauses)
  3. DDL operations (CREATE INDEX, DROP TABLE, DROP INDEX, ALTER TABLE)
  4. UNION NULL deduplication
  5. UNIQUE constraint NULL semantics
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    assert_no_error,
    assert_rows_equal,
    cleanup,
    minidb_query,
    run_minidb,
    temp_db,
)


def assert_eq(label, got, expected):
    if got != expected:
        raise AssertionError(f"{label}: got {got!r}, expected {expected!r}")


# ── Section 1: NULLS FIRST / NULLS LAST ────────────────────────────────

def test_nulls_ordering(bin_path, db, seed):
    """Test ORDER BY with NULLS FIRST / NULLS LAST modifiers."""

    # Setup table with mixed NULL / non-NULL data
    setup_out = run_minidb(bin_path, db, [
        "CREATE TABLE nullord (id INT PRIMARY KEY, val INT, name TEXT);",
        "INSERT INTO nullord VALUES (1, 30, 'alice');",
        "INSERT INTO nullord VALUES (2, NULL, 'bob');",
        "INSERT INTO nullord VALUES (3, 10, NULL);",
        "INSERT INTO nullord VALUES (4, NULL, 'dave');",
        "INSERT INTO nullord VALUES (5, 20, 'eve');",
        "INSERT INTO nullord VALUES (6, 10, 'frank');",
    ])
    assert_no_error(setup_out, seed, "nullord setup")

    # PostgreSQL default: ASC => NULLS LAST
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT id FROM nullord ORDER BY val ASC;", seed),
        [("3",), ("6",), ("5",), ("1",), ("2",), ("4",)],
        seed,
        "ASC default (NULLS LAST)",
    )

    # PostgreSQL default: DESC => NULLS FIRST
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT id FROM nullord ORDER BY val DESC;", seed),
        [("2",), ("4",), ("1",), ("5",), ("3",), ("6",)],
        seed,
        "DESC default (NULLS FIRST)",
    )

    # Explicit override: ASC NULLS FIRST
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT id FROM nullord ORDER BY val ASC NULLS FIRST;", seed),
        [("2",), ("4",), ("3",), ("6",), ("5",), ("1",)],
        seed,
        "ASC NULLS FIRST",
    )

    # Explicit override: DESC NULLS LAST
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT id FROM nullord ORDER BY val DESC NULLS LAST;", seed),
        [("1",), ("5",), ("3",), ("6",), ("2",), ("4",)],
        seed,
        "DESC NULLS LAST",
    )

    # Multi-column: first column ASC NULLS FIRST, second column DESC
    # For val=NULL group: name DESC with default NULLS FIRST → 'dave'(4) > 'bob'(2)
    # For val=10 group: name DESC with default NULLS FIRST → NULL(3) first, 'frank'(6)
    assert_rows_equal(
        minidb_query(
            bin_path, db,
            "SELECT id FROM nullord ORDER BY val ASC NULLS FIRST, name DESC;",
            seed,
        ),
        [("4",), ("2",), ("3",), ("6",), ("5",), ("1",)],
        seed,
        "multi-col: val ASC NULLS FIRST, name DESC",
    )

    # Multi-column: first column ASC, second column ASC NULLS FIRST (NULL name)
    assert_rows_equal(
        minidb_query(
            bin_path, db,
            "SELECT id FROM nullord ORDER BY val ASC, name ASC NULLS FIRST;",
            seed,
        ),
        [("3",), ("6",), ("5",), ("1",), ("2",), ("4",)],
        seed,
        "multi-col: val ASC, name ASC NULLS FIRST",
    )

    print("  [OK] NULLS FIRST / NULLS LAST")


# ── Section 2: CAST expression ──────────────────────────────────────────

def test_cast(bin_path, db, seed):
    """Test CAST between types, NULL propagation, and invalid casts."""

    setup_out = run_minidb(bin_path, db, [
        "CREATE TABLE castdata (id INT PRIMARY KEY, s TEXT, f FLOAT, b BOOL, i INT);",
        "INSERT INTO castdata VALUES (1, '42', 3.14, TRUE, 7);",
        "INSERT INTO castdata VALUES (2, '0', 0.0, FALSE, 0);",
        "INSERT INTO castdata VALUES (3, 'abc', 9.99, TRUE, 1);",
        "INSERT INTO castdata VALUES (4, NULL, NULL, NULL, NULL);",
    ])
    assert_no_error(setup_out, seed, "castdata setup")

    # CAST string to INT
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST('123' AS INT);", seed),
        [("123",)],
        seed,
        "CAST string literal to INT",
    )

    # CAST float to INT (truncation)
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(3.7 AS INT);", seed),
        [("3",)],
        seed,
        "CAST float literal to INT",
    )

    # CAST bool to INT
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(TRUE AS INT);", seed),
        [("1",)],
        seed,
        "CAST TRUE to INT",
    )

    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(FALSE AS INT);", seed),
        [("0",)],
        seed,
        "CAST FALSE to INT",
    )

    # CAST int to TEXT
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(99 AS TEXT);", seed),
        [("99",)],
        seed,
        "CAST int to TEXT",
    )

    # CAST float to TEXT (MiniDB uses %f → 6 decimal places)
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(3.14 AS TEXT);", seed),
        [("3.140000",)],
        seed,
        "CAST float to TEXT",
    )

    # CAST int to FLOAT (output uses %f → 6 decimal places)
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(5 AS FLOAT);", seed),
        [("5.000000",)],
        seed,
        "CAST int to FLOAT",
    )

    # CAST string to FLOAT
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST('2.718' AS FLOAT);", seed),
        [("2.718000",)],
        seed,
        "CAST string to FLOAT",
    )

    # CAST int to DOUBLE
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(5 AS DOUBLE);", seed),
        [("5.000000",)],
        seed,
        "CAST int to DOUBLE",
    )

    # CAST string to DOUBLE
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST('2.718' AS DOUBLE);", seed),
        [("2.718000",)],
        seed,
        "CAST string to DOUBLE",
    )

    # CAST int to BOOL (MiniDB displays bool as 1/0)
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(1 AS BOOL);", seed),
        [("1",)],
        seed,
        "CAST 1 to BOOL",
    )

    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(0 AS BOOL);", seed),
        [("0",)],
        seed,
        "CAST 0 to BOOL",
    )

    # CAST NULL preserves NULL
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(NULL AS INT);", seed),
        [("NULL",)],
        seed,
        "CAST NULL AS INT",
    )

    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST(NULL AS TEXT);", seed),
        [("NULL",)],
        seed,
        "CAST NULL AS TEXT",
    )

    # Invalid cast: non-numeric string to INT => NULL (safe failure)
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT CAST('abc' AS INT);", seed),
        [("NULL",)],
        seed,
        "CAST 'abc' AS INT => NULL",
    )

    # CAST column reference from table
    assert_rows_equal(
        minidb_query(bin_path, db,
                     "SELECT id, CAST(s AS INT) FROM castdata WHERE id <= 2 ORDER BY id;",
                     seed),
        [("1", "42"), ("2", "0")],
        seed,
        "CAST column s AS INT",
    )

    # CAST in WHERE clause
    assert_rows_equal(
        minidb_query(bin_path, db,
                     "SELECT id FROM castdata WHERE CAST(s AS INT) > 10 ORDER BY id;",
                     seed),
        [("1",)],
        seed,
        "CAST in WHERE clause",
    )

    # CAST in ORDER BY: 'abc' → NULL, NULLS FIRST is default for DESC
    # Order: NULL(3), 42(1), 0(2)
    assert_rows_equal(
        minidb_query(bin_path, db,
                     "SELECT id FROM castdata WHERE s IS NOT NULL ORDER BY CAST(s AS INT) DESC;",
                     seed),
        [("3",), ("1",), ("2",)],
        seed,
        "CAST in ORDER BY DESC (NULL first default)",
    )

    print("  [OK] CAST expression")


# ── Section 3: DDL operations ───────────────────────────────────────────

def test_ddl_operations(bin_path, db, seed):
    """Test DDL: CREATE INDEX, DROP INDEX, DROP TABLE, ALTER TABLE."""

    # Setup table with data
    setup_out = run_minidb(bin_path, db, [
        "CREATE TABLE ddl_t (id INT PRIMARY KEY, val INT, note TEXT);",
        "INSERT INTO ddl_t VALUES (1, 100, 'hello');",
        "INSERT INTO ddl_t VALUES (2, 200, 'world');",
        "INSERT INTO ddl_t VALUES (3, 300, 'foo');",
    ])
    assert_no_error(setup_out, seed, "ddl_t setup")

    # CREATE INDEX on populated table
    idx_out = run_minidb(bin_path, db, [
        "CREATE INDEX idx_ddl_val ON ddl_t(val);",
    ])
    assert_no_error(idx_out, seed, "CREATE INDEX on ddl_t")

    # Verify index works: query using the indexed column
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT id FROM ddl_t WHERE val = 200;", seed),
        [("2",)],
        seed,
        "query after CREATE INDEX",
    )

    # DROP INDEX
    drop_idx_out = run_minidb(bin_path, db, [
        "DROP INDEX idx_ddl_val;",
    ])
    assert_no_error(drop_idx_out, seed, "DROP INDEX")

    # Data still accessible after dropping index
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT id FROM ddl_t WHERE val = 200;", seed),
        [("2",)],
        seed,
        "query after DROP INDEX",
    )

    # ALTER TABLE ADD COLUMN
    alter_add_out = run_minidb(bin_path, db, [
        "ALTER TABLE ddl_t ADD COLUMN extra INT;",
    ])
    assert_no_error(alter_add_out, seed, "ALTER TABLE ADD COLUMN")

    # Existing rows should have NULL for the new column
    assert_rows_equal(
        minidb_query(bin_path, db,
                     "SELECT id, extra FROM ddl_t ORDER BY id;",
                     seed),
        [("1", "NULL"), ("2", "NULL"), ("3", "NULL")],
        seed,
        "existing rows have NULL in new column",
    )

    # Insert a row with the new column populated
    ins_out = run_minidb(bin_path, db, [
        "INSERT INTO ddl_t VALUES (4, 400, 'bar', 42);",
    ])
    assert_no_error(ins_out, seed, "INSERT with new column")

    assert_rows_equal(
        minidb_query(bin_path, db,
                     "SELECT id, extra FROM ddl_t WHERE id = 4;",
                     seed),
        [("4", "42")],
        seed,
        "new row has extra column value",
    )

    # ALTER TABLE DROP COLUMN
    alter_drop_out = run_minidb(bin_path, db, [
        "ALTER TABLE ddl_t DROP COLUMN extra;",
    ])
    assert_no_error(alter_drop_out, seed, "ALTER TABLE DROP COLUMN")

    # Verify the column is gone -- query remaining columns
    assert_rows_equal(
        minidb_query(bin_path, db,
                     "SELECT id, val FROM ddl_t ORDER BY id LIMIT 2;",
                     seed),
        [("1", "100"), ("2", "200")],
        seed,
        "data intact after DROP COLUMN",
    )

    # DROP TABLE
    drop_tbl_out = run_minidb(bin_path, db, [
        "DROP TABLE ddl_t;",
    ])
    assert_no_error(drop_tbl_out, seed, "DROP TABLE ddl_t")

    # Verify table is gone -- query should produce an error
    gone_out = run_minidb(bin_path, db, ["SELECT * FROM ddl_t;"])
    if "Error" not in gone_out:
        raise AssertionError(
            f"expected error querying dropped table ddl_t, got: {gone_out}"
        )

    print("  [OK] DDL operations")


# ── Section 4: UNION NULL deduplication ─────────────────────────────────

def test_union_null_dedup(bin_path, db, seed):
    """Test UNION / UNION ALL with NULL values."""

    # Setup helper table
    setup_out = run_minidb(bin_path, db, [
        "CREATE TABLE union_t (id INT, val INT);",
        "INSERT INTO union_t VALUES (1, 10);",
        "INSERT INTO union_t VALUES (2, NULL);",
        "INSERT INTO union_t VALUES (3, 10);",
        "INSERT INTO union_t VALUES (4, NULL);",
    ])
    assert_no_error(setup_out, seed, "union_t setup")

    # UNION deduplicates NULL: SELECT NULL UNION SELECT NULL => one NULL row
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT NULL UNION SELECT NULL;", seed),
        [("NULL",)],
        seed,
        "SELECT NULL UNION SELECT NULL => 1 row",
    )

    # UNION with mixed NULL and non-NULL
    rows = minidb_query(
        bin_path, db,
        "SELECT NULL UNION SELECT 1 UNION SELECT NULL UNION SELECT 2;",
        seed,
    )
    # Should have exactly 3 distinct values: NULL, 1, 2
    values = sorted([r[0] for r in rows])
    assert_eq("UNION NULL + values count", len(values), 3)
    assert "NULL" in values, f"NULL missing from UNION result: {values}"
    assert "1" in values, f"1 missing from UNION result: {values}"
    assert "2" in values, f"2 missing from UNION result: {values}"

    # UNION ALL preserves duplicate NULLs
    rows_all = minidb_query(
        bin_path, db,
        "SELECT NULL UNION ALL SELECT NULL;",
        seed,
    )
    assert_eq("UNION ALL NULL dup count", len(rows_all), 2)
    assert_eq("UNION ALL NULL dup values", rows_all, [("NULL",), ("NULL",)])

    # UNION ALL with table data preserving NULL dups
    rows_tbl = minidb_query(
        bin_path, db,
        "SELECT val FROM union_t WHERE val IS NULL "
        "UNION ALL SELECT val FROM union_t WHERE val IS NULL;",
        seed,
    )
    assert_eq("UNION ALL table NULL count", len(rows_tbl), 4)

    # UNION deduplicates across table queries
    rows_dedup = minidb_query(
        bin_path, db,
        "SELECT val FROM union_t UNION SELECT val FROM union_t;",
        seed,
    )
    # Distinct vals: 10, NULL => 2 rows
    assert_eq("UNION dedup table count", len(rows_dedup), 2)

    print("  [OK] UNION NULL deduplication")


# ── Section 5: UNIQUE constraint NULL semantics ─────────────────────────

def test_unique_null_semantics(bin_path, db, seed):
    """Multiple NULLs allowed in UNIQUE column; non-NULL dups rejected."""

    setup_out = run_minidb(bin_path, db, [
        "CREATE TABLE uniq_t (id INT PRIMARY KEY, code INT UNIQUE, label TEXT);",
        "INSERT INTO uniq_t VALUES (1, 100, 'a');",
        "INSERT INTO uniq_t VALUES (2, NULL, 'b');",
        "INSERT INTO uniq_t VALUES (3, NULL, 'c');",   # second NULL -- allowed
        "INSERT INTO uniq_t VALUES (4, NULL, 'd');",   # third NULL -- allowed
        "INSERT INTO uniq_t VALUES (5, 200, 'e');",
    ])
    assert_no_error(setup_out, seed, "uniq_t setup")

    # All five rows should be present
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT COUNT(*) FROM uniq_t;", seed),
        [("5",)],
        seed,
        "UNIQUE allows multiple NULLs (count)",
    )

    # Verify the NULL rows explicitly
    assert_rows_equal(
        minidb_query(bin_path, db,
                     "SELECT id FROM uniq_t WHERE code IS NULL ORDER BY id;",
                     seed),
        [("2",), ("3",), ("4",)],
        seed,
        "three NULL code rows present",
    )

    # Non-NULL duplicate must be rejected (MiniDB silently skips: affected_rows=0)
    dup_out = run_minidb(bin_path, db, [
        "INSERT INTO uniq_t VALUES (6, 100, 'f');",
    ])
    if "Error" not in dup_out and "affected_rows\n0" not in dup_out:
        raise AssertionError(
            f"expected duplicate rejection for UNIQUE value 100, got: {dup_out}"
        )

    # Verify row count unchanged after failed insert
    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT COUNT(*) FROM uniq_t;", seed),
        [("5",)],
        seed,
        "count unchanged after rejected dup",
    )

    # Another non-NULL duplicate
    dup2_out = run_minidb(bin_path, db, [
        "INSERT INTO uniq_t VALUES (7, 200, 'g');",
    ])
    if "Error" not in dup2_out and "affected_rows\n0" not in dup2_out:
        raise AssertionError(
            f"expected duplicate rejection for UNIQUE value 200, got: {dup2_out}"
        )

    # A fresh non-NULL value should succeed
    ok_out = run_minidb(bin_path, db, [
        "INSERT INTO uniq_t VALUES (8, 300, 'h');",
    ])
    assert_no_error(ok_out, seed, "insert unique 300")

    assert_rows_equal(
        minidb_query(bin_path, db, "SELECT COUNT(*) FROM uniq_t;", seed),
        [("6",)],
        seed,
        "count after inserting unique 300",
    )

    print("  [OK] UNIQUE NULL semantics")


# ── Main ────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser()
    add_seed_args(parser)
    args = parser.parse_args()
    seed = args.seed
    db = temp_db("minidb-p1-features.")
    try:
        test_nulls_ordering(args.bin, db, seed)
        test_cast(args.bin, db, seed)
        test_ddl_operations(args.bin, db, seed)
        test_union_null_dedup(args.bin, db, seed)
        test_unique_null_semantics(args.bin, db, seed)
        print(f"p1_features PASS seed={seed}")
        return 0
    except Exception as exc:
        print(f"p1_features FAIL seed={seed}: {exc}", file=sys.stderr)
        return 1
    finally:
        cleanup(db)


if __name__ == "__main__":
    raise SystemExit(main())
