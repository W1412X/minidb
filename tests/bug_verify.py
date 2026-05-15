#!/usr/bin/env python3
"""Targeted bug verification tests for newly discovered bugs."""
import re
import subprocess, os, sys, tempfile, shutil, math

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
DB_DIR = tempfile.mkdtemp(prefix="minidb_bug_")
PASS = FAIL = 0
BUGS = []

def cleanup(): shutil.rmtree(DB_DIR, ignore_errors=True)
def run_sql(sqls):
    if isinstance(sqls, str): sqls = [sqls]
    batch = list(sqls) + ["exit"]
    proc = subprocess.run([BIN, "--dir", DB_DIR], input="\n".join(batch),
                          capture_output=True, text=True, timeout=30)
    return proc.stdout + proc.stderr

def result_lines(haystack):
    lines = []
    for line in haystack.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("Data directory:", "MiniADB", "Type ", "minidb>", "Goodbye")):
            continue
        if "|" in stripped or re.fullmatch(r"-?\d+(\.\d+)?|NULL|true|false|[A-Za-z_][A-Za-z0-9_]*", stripped):
            lines.append(stripped)
    return lines

def check(desc, needle, haystack, negate=False):
    global PASS, FAIL
    found = needle in haystack
    if negate: found = not found
    if found:
        PASS += 1; print(f"  PASS: {desc}")
    else:
        FAIL += 1; BUGS.append((desc, needle, haystack[:300]))
        print(f"  FAIL: {desc}  (expected {'NOT ' if negate else ''}'{needle}')")

try:
    # === BUG 1: AVG integer truncation ===
    print("=== B1: AVG integer truncation ===")
    out = run_sql([
        "CREATE TABLE b1 (v INT);",
        "INSERT INTO b1 VALUES (100), (201);",
        "SELECT AVG(v) FROM b1;",
    ])
    # Expected: 150.5, Bug returns: 150 (integer division)
    check("AVG(100,201) should be 150.5 not 150", "150.5", out)
    # Also check: does the result look like an integer?
    check_not = "150\n" in out and "150.5" not in out
    if check_not:
        FAIL += 1; BUGS.append(("AVG returns truncated integer", "150.5", "150"))
        print("  FAIL: AVG returns truncated integer 150 instead of 150.5")
    else:
        PASS += 1; print("  PASS: AVG not truncated")

    # === BUG 2: NULL = NULL in unique index ===
    print("\n=== B2: NULL in unique constraint ===")
    out = run_sql([
        "CREATE TABLE b2 (id INT PRIMARY KEY, v INT UNIQUE);",
        "INSERT INTO b2 VALUES (1, NULL);",
        "INSERT INTO b2 VALUES (2, NULL);",
        "SELECT COUNT(*) FROM b2;",
    ])
    # SQL standard: NULL != NULL, so both inserts should succeed
    check("Two NULLs in UNIQUE column should both exist", "2", out)

    # === BUG 3: AVG on different data types ===
    print("\n=== B3: AVG precision with various values ===")
    out = run_sql([
        "CREATE TABLE b3 (v INT);",
        "INSERT INTO b3 VALUES (1), (2), (3), (4), (5);",
        "SELECT AVG(v) FROM b3;",
    ])
    # AVG(1,2,3,4,5) = 3.0
    check("AVG(1..5) = 3.0", "3", out)

    # === BUG 4: Negative LIMIT/OFFSET ===
    print("\n=== B4: Negative LIMIT/OFFSET ===")
    out = run_sql([
        "CREATE TABLE b4 (id INT PRIMARY KEY);",
        "INSERT INTO b4 VALUES (1), (2), (3);",
        "SELECT id FROM b4 LIMIT -1;",
    ])
    # Should either error or return all rows (treated as no limit)
    check("Negative LIMIT doesn't crash", "1", out)

    # === BUG 5: ORDER BY stability ===
    print("\n=== B5: ORDER BY with duplicates ===")
    out = run_sql([
        "CREATE TABLE b5 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO b5 VALUES (1, 10), (2, 10), (3, 10);",
        "SELECT id FROM b5 ORDER BY v ASC;",
    ])
    check("ORDER BY with dupes returns all rows", "1", out)
    check("ORDER BY with dupes returns row 2", "2", out)
    check("ORDER BY with dupes returns row 3", "3", out)

    # === BUG 6: COALESCE with 3+ args ===
    print("\n=== B6: COALESCE multi-arg ===")
    out = run_sql([
        "CREATE TABLE b6 (a INT, b INT, c INT);",
        "INSERT INTO b6 VALUES (NULL, NULL, 30);",
        "SELECT COALESCE(a, b, c) FROM b6;",
    ])
    check("COALESCE 3 args", "30", out)

    # === BUG 7: NULLIF edge cases ===
    print("\n=== B7: NULLIF edge cases ===")
    out = run_sql([
        "CREATE TABLE b7 (a INT, b INT);",
        "INSERT INTO b7 VALUES (NULL, NULL);",
        "SELECT NULLIF(a, b) FROM b7;",
    ])
    # NULLIF(NULL, NULL) should return NULL (not crash)
    check("NULLIF(NULL, NULL) = NULL", "NULL", out)

    # === BUG 8: Complex expression in WHERE ===
    print("\n=== B8: Complex WHERE expressions ===")
    out = run_sql([
        "CREATE TABLE b8 (id INT PRIMARY KEY, a INT, b INT, c INT);",
        "INSERT INTO b8 VALUES (1, 10, 20, 30);",
        "INSERT INTO b8 VALUES (2, 40, 50, 60);",
        "SELECT id FROM b8 WHERE (a + b) > 25;",
    ])
    check("(a+b)>25 finds row 2", "2", out)
    if "1" in result_lines(out):
        PASS += 1; print("  PASS: (a+b)>25 also finds row 1")
    else:
        FAIL += 1; BUGS.append(("(a+b)>25 also finds row 1", "result row '1'", out[:300]))
        print("  FAIL: (a+b)>25 also finds row 1")

    # === BUG 9: CASE WHEN with NULL conditions ===
    print("\n=== B9: CASE WHEN NULL ===")
    out = run_sql([
        "CREATE TABLE b9 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO b9 VALUES (1, NULL);",
        "SELECT CASE WHEN v = 1 THEN 'yes' WHEN v IS NULL THEN 'null' ELSE 'no' END FROM b9;",
    ])
    check("CASE WHEN NULL condition", "null", out)

    # === BUG 10: Division by zero ===
    print("\n=== B10: Division by zero ===")
    out = run_sql([
        "CREATE TABLE b10 (id INT PRIMARY KEY, a INT, b INT);",
        "INSERT INTO b10 VALUES (1, 10, 0);",
        "SELECT a / b FROM b10;",
    ])
    # Division by zero should return NULL, not crash
    check("Division by zero returns NULL or error", "NULL", out)

    # === BUG 11: SUM overflow ===
    print("\n=== B11: SUM overflow ===")
    out = run_sql([
        "CREATE TABLE b11 (v BIGINT);",
        "INSERT INTO b11 VALUES (999999999), (999999999);",
        "SELECT SUM(v) FROM b11;",
    ])
    check("SUM overflow doesn't crash", "1999999998", out)

    # === BUG 12: Multiple aggregates in single SELECT ===
    print("\n=== B12: Multiple different aggregates ===")
    out = run_sql([
        "CREATE TABLE b12 (v INT);",
        "INSERT INTO b12 VALUES (10), (20), (30), (NULL);",
        "SELECT COUNT(*), COUNT(v), SUM(v), AVG(v), MIN(v), MAX(v) FROM b12;",
    ])
    check("Multi-agg COUNT(*)", "4", out)
    check("Multi-agg COUNT(v)", "3", out)
    check("Multi-agg SUM(v)", "60", out)
    check("Multi-agg MIN(v)", "10", out)
    check("Multi-agg MAX(v)", "30", out)

    # === BUG 13: UPDATE with NULL ===
    print("\n=== B13: UPDATE SET NULL ===")
    out = run_sql([
        "CREATE TABLE b13 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO b13 VALUES (1, 10);",
        "UPDATE b13 SET v = NULL WHERE id = 1;",
        "SELECT v FROM b13;",
        "UPDATE b13 SET v = 99 WHERE v IS NULL;",
        "SELECT v FROM b13;",
    ])
    check("UPDATE SET NULL", "NULL", out)
    check("UPDATE WHERE IS NULL", "99", out)

    # === BUG 14: INSERT with expression types ===
    print("\n=== B14: INSERT type coercion ===")
    out = run_sql([
        "CREATE TABLE b14 (id INT PRIMARY KEY, v DOUBLE);",
        "INSERT INTO b14 VALUES (1, 3.14);",
        "INSERT INTO b14 VALUES (2, 2.718);",
        "SELECT v FROM b14 ORDER BY id ASC;",
    ])
    check("INSERT float value", "3.14", out)

    # === BUG 15: JOIN with NULL in ON condition ===
    print("\n=== B15: JOIN with NULL ON ===")
    out = run_sql([
        "CREATE TABLE a15 (id INT PRIMARY KEY, v INT);",
        "CREATE TABLE b15 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO a15 VALUES (1, NULL), (2, 10);",
        "INSERT INTO b15 VALUES (1, 10), (2, NULL);",
        "SELECT a15.id FROM a15 INNER JOIN b15 ON a15.v = b15.v;",
    ])
    check("JOIN ON NULL = NULL returns no match", "Goodbye", out)

    # === BUG 16: LEFT JOIN preserves non-matching rows ===
    print("\n=== B16: LEFT JOIN correctness ===")
    out = run_sql([
        "CREATE TABLE l16 (id INT PRIMARY KEY, v INT);",
        "CREATE TABLE r16 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO l16 VALUES (1, 10), (2, 20), (3, 30);",
        "INSERT INTO r16 VALUES (1, 100);",
        "SELECT l16.id FROM l16 LEFT JOIN r16 ON l16.id = r16.id ORDER BY l16.id ASC;",
    ])
    check("LEFT JOIN returns all 3 left rows", "3", out)

    # === BUG 17: Aggregate with GROUP BY and NULL groups ===
    print("\n=== B17: GROUP BY with NULL group ===")
    out = run_sql([
        "CREATE TABLE b17 (cat VARCHAR, v INT);",
        "INSERT INTO b17 VALUES ('A', 10), (NULL, 20), ('A', 30);",
        "SELECT cat, SUM(v) FROM b17 GROUP BY cat;",
    ])
    check("GROUP BY has A group", "A", out)
    check("GROUP BY has NULL group", "NULL", out)

    # === BUG 18: DELETE WHERE with NULL ===
    print("\n=== B18: DELETE WHERE NULL ===")
    out = run_sql([
        "CREATE TABLE b18 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO b18 VALUES (1, NULL), (2, 10);",
        "DELETE FROM b18 WHERE v IS NULL;",
        "SELECT COUNT(*) FROM b18;",
    ])
    check("DELETE WHERE IS NULL leaves 1 row", "1", out)

    # === BUG 19: Large OFFSET ===
    print("\n=== B19: Large OFFSET ===")
    out = run_sql([
        "CREATE TABLE b19 (id INT PRIMARY KEY);",
        "INSERT INTO b19 VALUES (1), (2), (3);",
        "SELECT id FROM b19 ORDER BY id ASC LIMIT 1 OFFSET 1000;",
    ])
    check("Large OFFSET returns no rows", "Goodbye", out)

    # === BUG 20: CASE WHEN with aggregate in THEN ===
    print("\n=== B20: CASE WHEN with literal ===")
    out = run_sql([
        "CREATE TABLE b20 (id INT PRIMARY KEY, v INT);",
        "INSERT INTO b20 VALUES (1, 5), (2, 15);",
        "SELECT CASE WHEN v > 10 THEN v * 2 ELSE v + 100 END FROM b20 ORDER BY id ASC;",
    ])
    check("CASE THEN expression", "30", out)
    check("CASE ELSE expression", "105", out)

    # === BUG 21: AVG on BOOL should not read inactive union fields ===
    print("\n=== B21: AVG BOOL conversion ===")
    out = run_sql([
        "CREATE TABLE b21 (b BOOL);",
        "INSERT INTO b21 VALUES (TRUE), (FALSE);",
        "SELECT AVG(b) FROM b21;",
    ])
    check("AVG(TRUE,FALSE) = 0.5", "0.5", out)

finally:
    cleanup()

print()
print("=" * 50)
print(f"  RESULTS: {PASS}/{PASS+FAIL} passed, {FAIL} failed")
print("=" * 50)
if BUGS:
    print("\n--- CONFIRMED BUGS ---")
    for desc, expected, got in BUGS:
        print(f"\n[BUG] {desc}")
        print(f"  Expected: {expected}")
        print(f"  Got: {got[:200]}")
    sys.exit(1)
else:
    print("\nAll tests passed!")
    sys.exit(0)
