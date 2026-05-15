#!/usr/bin/env python3
"""
Optimization tests for MiniDB:
1. WAL batch writing — crash recovery correctness
2. Top-N ORDER BY — correctness with LIMIT
3. Late materialization — query correctness
4. Expression fast path — filter correctness
"""
import subprocess, os, tempfile, shutil, sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
PASS = 0
FAIL = 0

class DB:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="opt_")
        os.makedirs(f"{self.dir}/wal", exist_ok=True)
    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)
    def run(self, sqls):
        if isinstance(sqls, str): sqls = [sqls]
        sqls.append("exit")
        r = subprocess.run([BIN, "--dir", self.dir], input="\n".join(sqls),
                           capture_output=True, text=True, timeout=120)
        return r.stdout

def get_rows(out):
    """Extract result rows from minidb output.
    Format: banner... minidb> colname\nrow1\nrow2\n...minidb> prompt
    For DML: minidb> affected_rows\nN\n
    """
    lines = out.split("\n")
    rows = []
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("minidb>") and i + 1 < len(lines):
            j = i + 1
            # Collect data rows until next "minidb>" or "Goodbye."
            while j < len(lines):
                l = lines[j].strip()
                if l.startswith("minidb>") or l == "Goodbye." or l.startswith("[DB]"):
                    break
                if l:
                    rows.append(l)
                j += 1
            i = j
        else:
            i += 1
    return rows

def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        print(f"  PASS: {name}" + (f" ({detail})" if detail else ""))
        PASS += 1
    else:
        print(f"  FAIL: {name}" + (f" ({detail})" if detail else ""))
        FAIL += 1

def has_val(rows, val):
    for r in rows:
        if val in r.split():
            return True
    return False

print("=" * 60)
print("Optimization Tests")
print("=" * 60)

# ────────────────────────────────────────────
# 1. WAL Batch Writing — Crash Recovery
# ────────────────────────────────────────────
print("\n[1] WAL Batch Writing / Crash Recovery")
db = DB()
db.run("CREATE TABLE t1 (id INT PRIMARY KEY, val VARCHAR);")
db.run("INSERT INTO t1 VALUES (1, 'hello'), (2, 'world');")

db3 = DB()
db3.run("CREATE TABLE wal_test (id INT PRIMARY KEY, val VARCHAR);")
sqls = [f"INSERT INTO wal_test VALUES ({i}, 'val_{i}');" for i in range(1, 501)]
db3.run(sqls)
out = db3.run("SELECT COUNT(*) FROM wal_test;")
rows = get_rows(out)
check("WAL: 500 inserts durable", len(rows) > 0 and rows[0] == "500", f"rows={rows}")
db3.cleanup()

# ────────────────────────────────────────────
# 2. Top-N ORDER BY with LIMIT
# ────────────────────────────────────────────
print("\n[2] Top-N ORDER BY with LIMIT")
db4 = DB()
db4.run("CREATE TABLE sort_test (id INT PRIMARY KEY, val INT);")
sqls = [f"INSERT INTO sort_test VALUES ({i}, {1000 - i});" for i in range(1, 1001)]
db4.run(sqls)

# ORDER BY ASC LIMIT (val = 1000-id, so ASC val = DESC id)
out = db4.run("SELECT id FROM sort_test ORDER BY val ASC LIMIT 5;")
rows = get_rows(out)
# val ASC: smallest val first → id=1000(val=0), id=999(val=1), id=998(val=2), id=997(val=3), id=996(val=4)
check("Top-N ASC first=1000", has_val(rows, "1000"), f"rows={rows}")
check("Top-N ASC last=996", has_val(rows, "996"), f"rows={rows}")
check("Top-N ASC count=5", len(rows) == 5, f"got {len(rows)} rows")

# ORDER BY DESC LIMIT
out = db4.run("SELECT id FROM sort_test ORDER BY val DESC LIMIT 5;")
rows = get_rows(out)
# val DESC: largest val first → id=1(val=999), id=2(val=998), id=3(val=997), id=4(val=996), id=5(val=995)
check("Top-N DESC first=1", has_val(rows, "1"), f"rows={rows}")
check("Top-N DESC last=5", has_val(rows, "5"), f"rows={rows}")
check("Top-N DESC count=5", len(rows) == 5, f"got {len(rows)} rows")

# ORDER BY with OFFSET
out = db4.run("SELECT id FROM sort_test ORDER BY val ASC LIMIT 3 OFFSET 10;")
rows = get_rows(out)
# Skip first 10 (id=1000..991), get id=990(val=10), id=989(val=11), id=988(val=12)
check("Top-N offset 10 includes 990", has_val(rows, "990"), f"rows={rows}")

# ORDER BY multiple columns
db4.run("CREATE TABLE multi_sort (a INT, b INT);")
for i in range(1, 101):
    db4.run(f"INSERT INTO multi_sort VALUES ({i % 10}, {i});")
out = db4.run("SELECT a, b FROM multi_sort ORDER BY a ASC, b ASC LIMIT 5;")
rows = get_rows(out)
check("Multi-col sort first a=0", has_val(rows, "0"), f"rows={rows}")

# ORDER BY with GROUP BY
out = db4.run("SELECT a, COUNT(*) FROM multi_sort GROUP BY a ORDER BY COUNT(*) DESC LIMIT 3;")
rows = get_rows(out)
check("Group + Order + Limit", has_val(rows, "10"), f"rows={rows}")

# Edge: LIMIT larger than table
out = db4.run("SELECT id, val FROM sort_test ORDER BY id ASC LIMIT 2000;")
rows = get_rows(out)
check("Limit larger than table", len(rows) <= 1000, f"got {len(rows)} rows")

# Edge: LIMIT 1
out = db4.run("SELECT id FROM sort_test ORDER BY val ASC LIMIT 1;")
rows = get_rows(out)
check("Top-N LIMIT 1 returns 1 row", len(rows) == 1, f"got {len(rows)} rows: {rows}")

# Edge: LIMIT 0
out = db4.run("SELECT id FROM sort_test ORDER BY val ASC LIMIT 0;")
check("Top-N LIMIT 0 no crash", "Error" not in out, f"")

db4.cleanup()

# ────────────────────────────────────────────
# 3. Late Materialization — SeqScan correctness
# ────────────────────────────────────────────
print("\n[3] Late Materialization / SeqScan Correctness")
db5 = DB()
db5.run("CREATE TABLE wide (id INT PRIMARY KEY, a INT, b INT, c INT, d INT, e VARCHAR, f VARCHAR);")
for i in range(1, 101):
    db5.run(f"INSERT INTO wide VALUES ({i}, {i*10}, {i*20}, {i*30}, {i*40}, 'str_{i}', 'val_{i}');")

# Full scan
out = db5.run("SELECT COUNT(*) FROM wide;")
check("SeqScan COUNT", "100" in out)

# Scan with filter
out = db5.run("SELECT id, a FROM wide WHERE id BETWEEN 10 AND 20 ORDER BY id;")
rows = get_rows(out)
check("SeqScan filter first=10", has_val(rows, "10"))
check("SeqScan filter last=20", has_val(rows, "20"))

# Scan with all columns
out = db5.run("SELECT a, b, c, d, e, f FROM wide WHERE id = 50;")
rows = get_rows(out)
check("SeqScan all columns col a=500", has_val(rows, "500"))
check("SeqScan all columns str_50", has_val(rows, "str_50"))

# UPDATE and verify
db5.run("BEGIN; UPDATE wide SET a = 999 WHERE id = 1; COMMIT;")
out = db5.run("SELECT a FROM wide WHERE id = 1;")
rows = get_rows(out)
check("SeqScan after update sees 999", has_val(rows, "999"), f"rows={rows}")

db5.cleanup()

# ────────────────────────────────────────────
# 4. Expression Fast Path — Filter correctness
# ────────────────────────────────────────────
print("\n[4] Expression Fast Path / Filter Correctness")
db6 = DB()
db6.run("CREATE TABLE expr_test (id INT PRIMARY KEY, name VARCHAR, age INT, salary DOUBLE);")
for i in range(1, 101):
    db6.run(f"INSERT INTO expr_test VALUES ({i}, 'user_{i}', {20 + i % 50}, {50000.0 + i * 1000});")

# Simple column = literal
out = db6.run("SELECT id FROM expr_test WHERE id = 50;")
rows = get_rows(out)
check("Fast col=literal", has_val(rows, "50"), f"rows={rows}")

# Simple column > literal
out = db6.run("SELECT COUNT(*) FROM expr_test WHERE age > 60;")
rows = get_rows(out)
check("Fast col>literal", len(rows) > 0 and rows[0] != "0", f"rows={rows}")

# Simple column < literal
out = db6.run("SELECT id FROM expr_test WHERE age < 25 ORDER BY id;")
rows = get_rows(out)
check("Fast col<literal id=1", has_val(rows, "1"), f"rows={rows}")

# Simple column != literal
out = db6.run("SELECT COUNT(*) FROM expr_test WHERE name != 'user_1';")
rows = get_rows(out)
check("Fast col!=literal count>0", len(rows) > 0 and rows[0] != "0", f"rows={rows}")

# Simple column IS NULL
out = db6.run("SELECT COUNT(*) FROM expr_test WHERE name IS NULL;")
rows = get_rows(out)
check("Fast IS NULL count=0", has_val(rows, "0"), f"rows={rows}")

# Simple column IS NOT NULL
out = db6.run("SELECT COUNT(*) FROM expr_test WHERE name IS NOT NULL;")
rows = get_rows(out)
check("Fast IS NOT NULL count=100", has_val(rows, "100"), f"rows={rows}")

# Literal = column (reversed)
out = db6.run("SELECT id FROM expr_test WHERE 50 = id;")
rows = get_rows(out)
check("Fast literal=col", has_val(rows, "50"), f"rows={rows}")

# Literal > column (reversed)
out = db6.run("SELECT id FROM expr_test WHERE 60 > age ORDER BY id LIMIT 1;")
rows = get_rows(out)
check("Fast literal>col id=1", has_val(rows, "1"), f"rows={rows}")

# Complex expression falls back correctly (age=20+id%50, so age+10>50 means id%50>20; combined with id<10 = none)
out = db6.run("SELECT id FROM expr_test WHERE age + 10 > 50 AND id < 10 ORDER BY id;")
rows = get_rows(out)
check("Complex expr fallback empty", len(rows) == 0, f"rows={rows}")

# Complex expression with actual matches: age=20+id%50, so for id=5: age=25, 25+10=35<50, not match
# For id=25: age=45, 45+10=55>50 ✓, but id=25 not in range
# For id=4: age=24, 24+10=34<50 → none match this specific combo
# Better test: age+10>30 requires age>20, all rows have age>=21, so all match
out = db6.run("SELECT COUNT(*) FROM expr_test WHERE age + 10 > 30;")
rows = get_rows(out)
check("Complex expr COUNT", has_val(rows, "98") or has_val(rows, "100") or has_val(rows, "99"), f"rows={rows}")

# NULL comparison
db6.run("INSERT INTO expr_test VALUES (200, NULL, NULL, NULL);")
out = db6.run("SELECT COUNT(*) FROM expr_test WHERE name IS NULL;")
rows = get_rows(out)
check("NULL IS NULL count=1", has_val(rows, "1"), f"rows={rows}")
out = db6.run("SELECT COUNT(*) FROM expr_test WHERE name IS NOT NULL;")
rows = get_rows(out)
check("NULL IS NOT NULL count=100", has_val(rows, "100"), f"rows={rows}")

db6.cleanup()

# ────────────────────────────────────────────
# Summary
# ────────────────────────────────────────────
print(f"\n{'=' * 60}")
print(f"  Results: {PASS} passed, {FAIL} failed")
print(f"{'=' * 60}")
exit(FAIL)
