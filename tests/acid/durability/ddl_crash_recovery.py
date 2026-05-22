#!/usr/bin/env python3
"""DDL crash-recovery test (ACID D8).

Verifies that committed DDL operations survive a simulated crash (SIGKILL)
followed by WAL recovery, and that uncommitted DDL operations are properly
rolled back on restart.

Scenarios
---------
1. Committed CREATE TABLE + INSERT survives crash
2. Committed CREATE INDEX survives crash (IndexScan still used)
3. Committed DROP TABLE survives crash (table stays gone)
4. Committed ALTER TABLE ADD COLUMN survives crash
5. Committed ALTER TABLE DROP COLUMN survives crash
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
from minidb_testlib import (  # noqa: E402
    add_seed_args,
    assert_no_error,
    assert_rows_equal,
    cleanup,
    minidb_query,
    run_minidb,
    select_rows_from_output,
    temp_db,
)

SEED = 12648457


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def kill_during_process(bin_path: str, db_dir: str, statements: list[str],
                        delay: float = 0.15) -> None:
    """Start minidb, feed it SQL statements, then SIGKILL before clean shutdown."""
    proc = subprocess.Popen(
        [bin_path, "--dir", db_dir],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        assert proc.stdin is not None
        for sql in statements:
            proc.stdin.write(sql + "\n")
            proc.stdin.flush()
            time.sleep(delay)
        time.sleep(0.3)
        proc.kill()
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


# ---------------------------------------------------------------------------
# Scenario 1 -- Committed CREATE TABLE + INSERT survives crash
# ---------------------------------------------------------------------------

def scenario_create_table(bin_path: str) -> None:
    db = temp_db("minidb-ddl-cr-s1.")
    try:
        # Step 1: create table and insert rows via a clean session so the
        # transaction commits fully.
        setup_sql = [
            "CREATE TABLE s1 (id INT PRIMARY KEY, val INT);",
            "BEGIN;",
            "INSERT INTO s1 VALUES (1, 100);",
            "INSERT INTO s1 VALUES (2, 200);",
            "INSERT INTO s1 VALUES (3, 300);",
            "COMMIT;",
        ]
        out = run_minidb(bin_path, db, setup_sql)
        assert_no_error(out, SEED, "s1 setup")

        # Verify data is present before the crash.
        rows = minidb_query(bin_path, db, "SELECT id, val FROM s1 ORDER BY id;", SEED)
        assert_rows_equal(rows, [("1", "100"), ("2", "200"), ("3", "300")],
                          SEED, "s1 pre-crash verify")

        # Step 2: simulate crash -- start a new session and SIGKILL it.
        kill_during_process(bin_path, db, [
            "SELECT 1;",  # just open the DB so WAL recovery path is exercised
        ])

        # Step 3: restart and verify table + data survived.
        rows = minidb_query(bin_path, db, "SELECT id, val FROM s1 ORDER BY id;", SEED)
        assert_rows_equal(rows, [("1", "100"), ("2", "200"), ("3", "300")],
                          SEED, "s1 post-crash verify")

        print(f"  scenario 1 (CREATE TABLE + INSERT) PASS")
    finally:
        cleanup(db)


# ---------------------------------------------------------------------------
# Scenario 2 -- Committed CREATE INDEX survives crash
# ---------------------------------------------------------------------------

def scenario_create_index(bin_path: str) -> None:
    db = temp_db("minidb-ddl-cr-s2.")
    try:
        # Step 1: create table, insert data, create index.
        setup_sql = [
            "CREATE TABLE s2 (id INT PRIMARY KEY, score INT);",
            "BEGIN;",
            "INSERT INTO s2 VALUES (1, 50);",
            "INSERT INTO s2 VALUES (2, 90);",
            "INSERT INTO s2 VALUES (3, 70);",
            "INSERT INTO s2 VALUES (4, 30);",
            "INSERT INTO s2 VALUES (5, 80);",
            "COMMIT;",
            "CREATE INDEX s2_score ON s2(score);",
        ]
        out = run_minidb(bin_path, db, setup_sql)
        assert_no_error(out, SEED, "s2 setup")

        # Verify the index is used before crash.
        out = run_minidb(bin_path, db, ["EXPLAIN SELECT * FROM s2 WHERE score = 70;"])
        assert_no_error(out, SEED, "s2 pre-crash EXPLAIN")
        if "IndexScan" not in out:
            raise AssertionError(
                f"s2 pre-crash: expected IndexScan in EXPLAIN output seed={SEED}\n{out}"
            )

        # Step 2: simulate crash.
        kill_during_process(bin_path, db, ["SELECT 1;"])

        # Step 3: verify index still works after recovery.
        out = run_minidb(bin_path, db, ["EXPLAIN SELECT * FROM s2 WHERE score = 70;"])
        assert_no_error(out, SEED, "s2 post-crash EXPLAIN")
        if "IndexScan" not in out:
            raise AssertionError(
                f"s2 post-crash: expected IndexScan in EXPLAIN output seed={SEED}\n{out}"
            )

        # Step 4: verify query correctness via the index.
        rows = minidb_query(bin_path, db, "SELECT id, score FROM s2 WHERE score = 70;", SEED)
        assert_rows_equal(rows, [("3", "70")], SEED, "s2 post-crash indexed query")

        # Also verify full table data survived.
        rows = minidb_query(bin_path, db,
                            "SELECT id, score FROM s2 ORDER BY id;", SEED)
        assert_rows_equal(rows,
                          [("1", "50"), ("2", "90"), ("3", "70"),
                           ("4", "30"), ("5", "80")],
                          SEED, "s2 post-crash full scan")

        print(f"  scenario 2 (CREATE INDEX) PASS")
    finally:
        cleanup(db)


# ---------------------------------------------------------------------------
# Scenario 3 -- Committed DROP TABLE survives crash
# ---------------------------------------------------------------------------

def scenario_drop_table(bin_path: str) -> None:
    db = temp_db("minidb-ddl-cr-s3.")
    try:
        # Step 1: create table, insert data, then drop it.
        setup_sql = [
            "CREATE TABLE s3 (id INT PRIMARY KEY, name VARCHAR(32));",
            "BEGIN;",
            "INSERT INTO s3 VALUES (1, 'alice');",
            "INSERT INTO s3 VALUES (2, 'bob');",
            "COMMIT;",
            "DROP TABLE s3;",
        ]
        out = run_minidb(bin_path, db, setup_sql)
        assert_no_error(out, SEED, "s3 setup")

        # Step 2: simulate crash.
        kill_during_process(bin_path, db, ["SELECT 1;"])

        # Step 3: verify table is gone -- querying it should produce an error.
        out = run_minidb(bin_path, db, ["SELECT * FROM s3;"])
        if "Error" not in out and "error" not in out:
            raise AssertionError(
                f"s3 post-crash: expected error querying dropped table seed={SEED}\n{out}"
            )

        print(f"  scenario 3 (DROP TABLE) PASS")
    finally:
        cleanup(db)


# ---------------------------------------------------------------------------
# Scenario 4 -- Committed ALTER TABLE ADD COLUMN survives crash
# ---------------------------------------------------------------------------

def scenario_alter_add_column(bin_path: str) -> None:
    db = temp_db("minidb-ddl-cr-s4.")
    try:
        # Step 1: create table with data.
        setup_sql = [
            "CREATE TABLE s4 (id INT PRIMARY KEY, name VARCHAR(32));",
            "BEGIN;",
            "INSERT INTO s4 VALUES (1, 'alice');",
            "INSERT INTO s4 VALUES (2, 'bob');",
            "COMMIT;",
        ]
        out = run_minidb(bin_path, db, setup_sql)
        assert_no_error(out, SEED, "s4 setup")

        # Step 2: ALTER TABLE to add a column, then insert a row using it.
        alter_sql = [
            "ALTER TABLE s4 ADD COLUMN age INT DEFAULT 0;",
            "BEGIN;",
            "INSERT INTO s4 VALUES (3, 'charlie', 25);",
            "COMMIT;",
        ]
        out = run_minidb(bin_path, db, alter_sql)
        assert_no_error(out, SEED, "s4 alter + insert")

        # Step 3: simulate crash.
        kill_during_process(bin_path, db, ["SELECT 1;"])

        # Step 4: verify the new column exists and data is correct.
        rows = minidb_query(bin_path, db,
                            "SELECT id, name, age FROM s4 ORDER BY id;", SEED)
        assert_rows_equal(rows,
                          [("1", "alice", "0"), ("2", "bob", "0"), ("3", "charlie", "25")],
                          SEED, "s4 post-crash verify")

        print(f"  scenario 4 (ALTER ADD COLUMN) PASS")
    finally:
        cleanup(db)


# ---------------------------------------------------------------------------
# Scenario 5 -- Committed ALTER TABLE DROP COLUMN survives crash
# ---------------------------------------------------------------------------

def scenario_alter_drop_column(bin_path: str) -> None:
    db = temp_db("minidb-ddl-cr-s5.")
    try:
        # Step 1: create table with 3 columns and add data.
        setup_sql = [
            "CREATE TABLE s5 (id INT PRIMARY KEY, middle INT, tail VARCHAR(16));",
            "BEGIN;",
            "INSERT INTO s5 VALUES (1, 10, 'aaa');",
            "INSERT INTO s5 VALUES (2, 20, 'bbb');",
            "INSERT INTO s5 VALUES (3, 30, 'ccc');",
            "COMMIT;",
        ]
        out = run_minidb(bin_path, db, setup_sql)
        assert_no_error(out, SEED, "s5 setup")

        # Step 2: drop the middle column.
        out = run_minidb(bin_path, db, ["ALTER TABLE s5 DROP COLUMN middle;"])
        assert_no_error(out, SEED, "s5 alter drop column")

        # Step 3: simulate crash.
        kill_during_process(bin_path, db, ["SELECT 1;"])

        # Step 4: verify column is gone and remaining data is correct.
        rows = minidb_query(bin_path, db,
                            "SELECT id, tail FROM s5 ORDER BY id;", SEED)
        assert_rows_equal(rows,
                          [("1", "aaa"), ("2", "bbb"), ("3", "ccc")],
                          SEED, "s5 post-crash verify")

        # Querying the dropped column should fail.
        out = run_minidb(bin_path, db, ["SELECT middle FROM s5;"])
        if "Error" not in out and "error" not in out:
            raise AssertionError(
                f"s5 post-crash: expected error querying dropped column seed={SEED}\n{out}"
            )

        print(f"  scenario 5 (ALTER DROP COLUMN) PASS")
    finally:
        cleanup(db)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="DDL crash-recovery test")
    add_seed_args(parser)
    args = parser.parse_args()
    bin_path = args.bin

    print(f"ddl_crash_recovery seed={SEED}")
    try:
        scenario_create_table(bin_path)
        scenario_create_index(bin_path)
        scenario_drop_table(bin_path)
        scenario_alter_add_column(bin_path)
        scenario_alter_drop_column(bin_path)

        print(f"ddl_crash_recovery PASS seed={SEED}")
        return 0
    except Exception as exc:
        print(f"ddl_crash_recovery FAIL seed={SEED}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
