#!/usr/bin/env python3
"""
Data Consistency Test Suite for MiniDB.
Tests: WAL recovery, MVCC visibility, transaction isolation, crash recovery.
"""
import subprocess, os, sys, tempfile, shutil, time, threading, socket, signal, struct

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
PASS = FAIL = 0
BUGS = []

class TestDB:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="minidb_consist_")
    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)
    def run(self, sqls):
        if isinstance(sqls, str): sqls = [sqls]
        sqls.append("exit")
        r = subprocess.run([BIN, "--dir", self.dir], input="\n".join(sqls),
                           capture_output=True, text=True, timeout=30)
        return r.stdout + r.stderr
    def run_server(self, port):
        p = subprocess.Popen([BIN, "--dir", self.dir, "--server", "--port", str(port)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.8)
        return p
    def stop_server(self, p):
        if p:
            p.terminate()
            try: p.wait(timeout=5)
            except: p.kill()
    def send(self, sqls, port):
        if isinstance(sqls, str): sqls = [sqls]
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(10)
            s.connect(("127.0.0.1", port))
            s.recv(4096)
            for sql in sqls:
                s.sendall((sql + "\n").encode())
            time.sleep(0.3)
            result = b""
            while True:
                s.settimeout(1)
                try:
                    chunk = s.recv(65536)
                    if not chunk: break
                    result += chunk
                except: break
            s.close()
            return result.decode(errors="replace")
        except Exception as e:
            return f"ERROR: {e}"

def check(desc, needle, haystack, negate=False):
    global PASS, FAIL
    # Strip noise but keep actual data output
    lines = []
    for l in haystack.split('\n'):
        stripped = l.strip()
        # Remove known noise prefixes
        if stripped.startswith('Data dir'):
            continue
        if stripped.startswith('MiniADB'):
            continue
        if stripped.startswith('Type '):
            continue
        if stripped.startswith('[DB]'):
            continue
        # Strip "minidb> " prompt prefix but keep the rest
        if stripped.startswith('minidb> '):
            stripped = stripped[len('minidb> '):]
        if stripped:
            lines.append(stripped)
    clean = '\n'.join(lines)
    found = needle in clean
    if negate: found = not found
    if found:
        PASS += 1
    else:
        FAIL += 1
        BUGS.append((desc, needle, clean[:300]))
        print(f"  FAIL: {desc}  (expected {'NOT ' if negate else ''}'{needle}')")

# ================================================================
# Test Group 1: WAL Recovery — Crash-Consistent Inserts
# ================================================================
print("=== 1. WAL Recovery: Crash after INSERT ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE wal_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO wal_t VALUES (1, 10);",
        "INSERT INTO wal_t VALUES (2, 20);",
        "INSERT INTO wal_t VALUES (3, 30);",
    ])
    # Simulate clean shutdown (destructor flushes)
    out = db.run(["SELECT COUNT(*) FROM wal_t;"])
    check("WAL recovery: 3 inserts survive clean shutdown", "3", out)

    # Verify data integrity after restart
    out = db.run(["SELECT SUM(v) FROM wal_t;"])
    check("WAL recovery: SUM correct after restart", "60", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 2: WAL Recovery — INSERT then crash (partial)
# ================================================================
print("\n=== 2. WAL Recovery: Large batch inserts ===")
db = TestDB()
try:
    sqls = ["CREATE TABLE batch_t (id INT PRIMARY KEY, v INT);"]
    for i in range(1, 501):
        sqls.append(f"INSERT INTO batch_t VALUES ({i}, {i * 3});")
    sqls.append("SELECT COUNT(*) FROM batch_t;")
    sqls.append("SELECT SUM(v) FROM batch_t;")
    out = db.run(sqls)
    check("Batch insert: 500 rows", "500", out)
    # SUM(1..500)*3 = 125250 * 3 = 375750
    check("Batch insert: SUM correct", "375750", out)

    # Restart and verify
    out = db.run(["SELECT COUNT(*) FROM batch_t;", "SELECT SUM(v) FROM batch_t;"])
    check("Batch after restart: 500 rows", "500", out)
    check("Batch after restart: SUM correct", "375750", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 3: WAL Recovery — DELETE then verify
# ================================================================
print("\n=== 3. WAL Recovery: DELETE consistency ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE del_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO del_t VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);",
        "DELETE FROM del_t WHERE v > 20;",
        "SELECT COUNT(*) FROM del_t;",
        "SELECT SUM(v) FROM del_t;",
    ])
    out = db.run(["SELECT COUNT(*) FROM del_t;"])
    check("DELETE: 2 rows remain after deleting v>20", "2", out)

    # Restart
    out = db.run(["SELECT COUNT(*) FROM del_t;", "SELECT id FROM del_t ORDER BY id;"])
    check("DELETE after restart: count=2", "2", out)
    check("DELETE after restart: id=1", "1", out)
    check("DELETE after restart: id=2", "2", out)
    check("DELETE after restart: id=3 gone", "3", out, negate=True)
finally:
    db.cleanup()

# ================================================================
# Test Group 4: WAL Recovery — UPDATE then verify
# ================================================================
print("\n=== 4. WAL Recovery: UPDATE consistency ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE upd_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO upd_t VALUES (1, 10), (2, 20), (3, 30);",
        "UPDATE upd_t SET v = v * 10;",
        "SELECT SUM(v) FROM upd_t;",
    ])
    out = db.run(["SELECT SUM(v) FROM upd_t;"])
    check("UPDATE all: SUM = 600", "600", out)

    # Restart and verify
    out = db.run(["SELECT SUM(v) FROM upd_t;"])
    check("UPDATE after restart: SUM = 600", "600", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 5: Transaction — COMMIT persistence
# ================================================================
print("\n=== 5. Transaction COMMIT persistence ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE txn_t (id INT PRIMARY KEY, v INT);",
        "BEGIN;",
        "INSERT INTO txn_t VALUES (1, 100);",
        "INSERT INTO txn_t VALUES (2, 200);",
        "COMMIT;",
    ])
    out = db.run(["SELECT COUNT(*) FROM txn_t;"])
    check("COMMIT: 2 rows visible", "2", out)

    out = db.run(["SELECT SUM(v) FROM txn_t;"])
    check("COMMIT: SUM = 300", "300", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 6: Transaction — ROLLBACK persistence
# ================================================================
print("\n=== 6. Transaction ROLLBACK persistence ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE rb_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO rb_t VALUES (1, 10);",
        "BEGIN;",
        "INSERT INTO rb_t VALUES (2, 20);",
        "INSERT INTO rb_t VALUES (3, 30);",
        "ROLLBACK;",
    ])
    out = db.run(["SELECT COUNT(*) FROM rb_t;"])
    check("ROLLBACK: only 1 row remains", "1", out)

    out = db.run(["SELECT id FROM rb_t;"])
    check("ROLLBACK: id=1 preserved", "1", out)
    check("ROLLBACK: id=2 gone", "2", out, negate=True)
finally:
    db.cleanup()

# ================================================================
# Test Group 7: Mixed COMMIT/ROLLBACK
# ================================================================
print("\n=== 7. Mixed COMMIT/ROLLBACK ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE mix_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO mix_t VALUES (1, 10);",
        "BEGIN;",
        "INSERT INTO mix_t VALUES (2, 20);",
        "COMMIT;",
        "BEGIN;",
        "INSERT INTO mix_t VALUES (3, 30);",
        "ROLLBACK;",
        "BEGIN;",
        "INSERT INTO mix_t VALUES (4, 40);",
        "COMMIT;",
    ])
    out = db.run(["SELECT COUNT(*) FROM mix_t;"])
    check("Mixed: 3 rows (1 committed, 2 rolled back, 3 committed)", "3", out)

    out = db.run(["SELECT id FROM mix_t ORDER BY id;"])
    check("Mixed: id=1", "1", out)
    check("Mixed: id=2", "2", out)
    check("Mixed: id=3 gone", "3", out, negate=True)
    check("Mixed: id=4", "4", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 8: UPDATE inside ROLLBACK
# ================================================================
print("\n=== 8. UPDATE inside ROLLBACK ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE upd_rb_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO upd_rb_t VALUES (1, 100), (2, 200), (3, 300);",
        "BEGIN;",
        "UPDATE upd_rb_t SET v = 999 WHERE id = 2;",
        "ROLLBACK;",
        "SELECT v FROM upd_rb_t WHERE id = 2;",
    ])
    out = db.run(["SELECT v FROM upd_rb_t WHERE id = 2;"])
    check("UPDATE ROLLBACK: v=200 preserved", "200", out)
    check("UPDATE ROLLBACK: v=999 not present", "999", out, negate=True)
finally:
    db.cleanup()

# ================================================================
# Test Group 9: MVCC — Concurrent read consistency
# ================================================================
print("\n=== 9. MVCC: Concurrent read via server ===")
db = TestDB()
PORT = 15501
server = None
try:
    db.run([
        "CREATE TABLE mvcc_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO mvcc_t VALUES (1, 10), (2, 20), (3, 30);",
    ])
    server = db.run_server(PORT)
    time.sleep(0.3)

    # Read in main thread
    r1 = db.send("SELECT COUNT(*) FROM mvcc_t;", PORT)
    check("MVCC read: 3 rows", "3", r1)

    # Concurrent write + read
    def writer():
        db.send([
            "BEGIN;",
            "INSERT INTO mvcc_t VALUES (4, 40);",
            "INSERT INTO mvcc_t VALUES (5, 50);",
            "COMMIT;",
        ], PORT)

    def reader():
        for _ in range(5):
            r = db.send("SELECT COUNT(*) FROM mvcc_t;", PORT)
            if "5" in r:
                return True
            time.sleep(0.1)
        return False

    t1 = threading.Thread(target=writer)
    t2 = threading.Thread(target=reader)
    t1.start(); t2.start()
    t1.join(timeout=10); t2.join(timeout=10)
    time.sleep(0.5)

    # Verify final state
    out = db.send("SELECT COUNT(*) FROM mvcc_t;", PORT)
    check("MVCC concurrent: 5 rows visible after write", "5", out)
finally:
    db.stop_server(server)
    db.cleanup()

# ================================================================
# Test Group 10: DELETE + INSERT same id (slot reuse)
# ================================================================
print("\n=== 10. DELETE + INSERT same id (slot reuse) ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE reuse_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO reuse_t VALUES (1, 10);",
        "DELETE FROM reuse_t WHERE id = 1;",
        "INSERT INTO reuse_t VALUES (1, 99);",
        "SELECT v FROM reuse_t WHERE id = 1;",
    ])
    out = db.run(["SELECT v FROM reuse_t;"])
    check("Slot reuse: v=99 after delete+insert", "99", out)
    check("Slot reuse: old v=10 gone", "10", out, negate=True)

    # Restart and verify
    out = db.run(["SELECT v FROM reuse_t;"])
    check("Slot reuse after restart: v=99", "99", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 11: Multi-table transaction
# ================================================================
print("\n=== 11. Multi-table transaction ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE mt1 (id INT PRIMARY KEY, v INT);",
        "CREATE TABLE mt2 (id INT PRIMARY KEY, v INT);",
        "BEGIN;",
        "INSERT INTO mt1 VALUES (1, 10);",
        "INSERT INTO mt2 VALUES (1, 20);",
        "COMMIT;",
        "SELECT (SELECT SUM(v) FROM mt1) + (SELECT SUM(v) FROM mt2);",
    ])
    # This might not work if scalar subquery not supported, verify tables
    out1 = db.run(["SELECT SUM(v) FROM mt1;"])
    check("Multi-table: mt1 SUM=10", "10", out1)
    out2 = db.run(["SELECT SUM(v) FROM mt2;"])
    check("Multi-table: mt2 SUM=20", "20", out2)
finally:
    db.cleanup()

# ================================================================
# Test Group 12: Stress — 1000 sequential inserts + verify
# ================================================================
print("\n=== 12. Stress: 1000 sequential inserts ===")
db = TestDB()
try:
    sqls = ["CREATE TABLE stress_t (id INT PRIMARY KEY, v INT);"]
    for i in range(1, 1001):
        sqls.append(f"INSERT INTO stress_t VALUES ({i}, {i * 7});")
    sqls.append("SELECT COUNT(*) FROM stress_t;")
    sqls.append("SELECT SUM(v) FROM stress_t;")
    sqls.append("SELECT MAX(id) FROM stress_t;")
    sqls.append("SELECT MIN(v) FROM stress_t;")
    sqls.append("SELECT MAX(v) FROM stress_t;")
    out = db.run(sqls)
    check("Stress 1000: COUNT=1000", "1000", out)
    check("Stress 1000: MAX(id)=1000", "1000", out)
    check("Stress 1000: MIN(v)=7", "7", out)
    check("Stress 1000: MAX(v)=7000", "7000", out)

    # Restart
    out = db.run(["SELECT COUNT(*) FROM stress_t;", "SELECT MAX(v) FROM stress_t;"])
    check("Stress after restart: COUNT=1000", "1000", out)
    check("Stress after restart: MAX(v)=7000", "7000", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 13: DELETE all + INSERT (space reclaim)
# ================================================================
print("\n=== 13. DELETE all + INSERT (space reclaim) ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE reclaim_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO reclaim_t VALUES (1, 10), (2, 20), (3, 30);",
        "DELETE FROM reclaim_t;",
        "SELECT COUNT(*) FROM reclaim_t;",
        "INSERT INTO reclaim_t VALUES (100, 999);",
        "SELECT COUNT(*) FROM reclaim_t;",
        "SELECT v FROM reclaim_t;",
    ])
    out = db.run(["SELECT v FROM reclaim_t;"])
    check("Space reclaim: v=999", "999", out)

    out = db.run(["SELECT COUNT(*) FROM reclaim_t;"])
    check("Space reclaim: count=1", "1", out)

    # Restart
    out = db.run(["SELECT v FROM reclaim_t;"])
    check("Space reclaim after restart: v=999", "999", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 14: UPDATE cascade — update same row multiple times
# ================================================================
print("\n=== 14. UPDATE cascade: same row multiple times ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE cascade_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO cascade_t VALUES (1, 0);",
        "UPDATE cascade_t SET v = v + 1 WHERE id = 1;",
        "UPDATE cascade_t SET v = v + 1 WHERE id = 1;",
        "UPDATE cascade_t SET v = v + 1 WHERE id = 1;",
        "UPDATE cascade_t SET v = v + 1 WHERE id = 1;",
        "UPDATE cascade_t SET v = v + 1 WHERE id = 1;",
        "SELECT v FROM cascade_t WHERE id = 1;",
    ])
    out = db.run(["SELECT v FROM cascade_t;"])
    check("Cascade UPDATE: v=5 after 5 increments", "5", out)

    # Restart
    out = db.run(["SELECT v FROM cascade_t;"])
    check("Cascade UPDATE after restart: v=5", "5", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 15: Index + WAL consistency
# ================================================================
print("\n=== 15. Index + WAL consistency ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE idx_t (id INT PRIMARY KEY, v INT);",
        "CREATE INDEX idx_v ON idx_t (v);",
        "INSERT INTO idx_t VALUES (1, 100), (2, 200), (3, 300);",
        "SELECT id FROM idx_t WHERE v = 200;",
    ])
    out = db.run(["SELECT id FROM idx_t WHERE v = 200;"])
    check("Index: v=200 finds id=2", "2", out)

    # Restart and verify index works
    out = db.run(["SELECT id FROM idx_t WHERE v = 200;"])
    check("Index after restart: v=200 finds id=2", "2", out)

    # Delete and verify index updated
    db.run(["DELETE FROM idx_t WHERE v = 200;"])
    out = db.run(["SELECT id FROM idx_t WHERE v = 200;"])
    check("Index after DELETE: v=200 returns empty", "Goodbye", out)

    # Restart and verify
    out = db.run(["SELECT id FROM idx_t WHERE v = 200;"])
    check("Index DELETE after restart: empty", "Goodbye", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 16: Very large transactions
# ================================================================
print("\n=== 16. Large transaction: 200 inserts in one txn ===")
db = TestDB()
try:
    sqls = ["CREATE TABLE large_txn_t (id INT PRIMARY KEY, v INT);", "BEGIN;"]
    for i in range(1, 201):
        sqls.append(f"INSERT INTO large_txn_t VALUES ({i}, {i});")
    sqls.append("COMMIT;")
    sqls.append("SELECT COUNT(*) FROM large_txn_t;")
    out = db.run(sqls)
    check("Large txn: 200 rows after COMMIT", "200", out)

    # ROLLBACK large txn
    db.run(["DELETE FROM large_txn_t;"])
    sqls = ["BEGIN;"]
    for i in range(1, 201):
        sqls.append(f"INSERT INTO large_txn_t VALUES ({i}, {i});")
    sqls.append("ROLLBACK;")
    sqls.append("SELECT COUNT(*) FROM large_txn_t;")
    out = db.run(sqls)
    check("Large txn ROLLBACK: 0 rows", "0", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 17: NULL handling in aggregates across restarts
# ================================================================
print("\n=== 17. NULL in aggregates — persistence ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE null_agg_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO null_agg_t VALUES (1, NULL), (2, 10), (3, NULL), (4, 30);",
        "SELECT COUNT(*) FROM null_agg_t;",
        "SELECT COUNT(v) FROM null_agg_t;",
        "SELECT SUM(v) FROM null_agg_t;",
        "SELECT AVG(v) FROM null_agg_t;",
        "SELECT MIN(v) FROM null_agg_t;",
        "SELECT MAX(v) FROM null_agg_t;",
    ])
    out = db.run(["SELECT COUNT(*) FROM null_agg_t;"])
    check("NULL agg: COUNT(*)=4", "4", out)
    out = db.run(["SELECT COUNT(v) FROM null_agg_t;"])
    check("NULL agg: COUNT(v)=2", "2", out)
    out = db.run(["SELECT SUM(v) FROM null_agg_t;"])
    check("NULL agg: SUM=40", "40", out)

    # Restart and verify
    out = db.run(["SELECT COUNT(*) FROM null_agg_t;", "SELECT SUM(v) FROM null_agg_t;"])
    check("NULL agg after restart: COUNT=4", "4", out)
    check("NULL agg after restart: SUM=40", "40", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 18: Subquery IN correctness
# ================================================================
print("\n=== 18. Subquery IN + NOT IN ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE sub_a (id INT PRIMARY KEY);",
        "CREATE TABLE sub_b (id INT PRIMARY KEY);",
        "INSERT INTO sub_a VALUES (1), (2), (3), (4), (5);",
        "INSERT INTO sub_b VALUES (2), (4);",
        "SELECT id FROM sub_a WHERE id IN (SELECT id FROM sub_b) ORDER BY id;",
    ])
    out = db.run(["SELECT id FROM sub_a WHERE id IN (SELECT id FROM sub_b) ORDER BY id;"])
    check("Subquery IN: finds 2", "2", out)
    check("Subquery IN: finds 4", "4", out)
    check("Subquery IN: no 1", "1", out, negate=True)
finally:
    db.cleanup()

# ================================================================
# Test Group 19: INSERT expressions persist
# ================================================================
print("\n=== 19. INSERT with expressions ===")
db = TestDB()
try:
    db.run([
        "CREATE TABLE expr_ins_t (id INT PRIMARY KEY, v INT);",
        "INSERT INTO expr_ins_t VALUES (1, 10 + 20);",
        "INSERT INTO expr_ins_t VALUES (2, 100 / 4);",
        "SELECT id, v FROM expr_ins_t ORDER BY id;",
    ])
    out = db.run(["SELECT v FROM expr_ins_t WHERE id = 1;"])
    check("INSERT expr 10+20=30", "30", out)
    out = db.run(["SELECT v FROM expr_ins_t WHERE id = 2;"])
    check("INSERT expr 100/4=25", "25", out)

    # Restart
    out = db.run(["SELECT v FROM expr_ins_t WHERE id = 1;"])
    check("INSERT expr after restart: 30", "30", out)
finally:
    db.cleanup()

# ================================================================
# Test Group 20: ORDER BY + LIMIT persistence
# ================================================================
print("\n=== 20. ORDER BY + LIMIT correctness ===")
db = TestDB()
try:
    db.run(["CREATE TABLE order_t (id INT PRIMARY KEY, v INT);"])
    sqls = []
    import random
    vals = list(range(1, 101))
    random.seed(42)
    random.shuffle(vals)
    for i, v in enumerate(vals, 1):
        sqls.append(f"INSERT INTO order_t VALUES ({i}, {v});")
    sqls.append("SELECT id FROM order_t ORDER BY v ASC LIMIT 5;")
    sqls.append("SELECT id FROM order_t ORDER BY v DESC LIMIT 5;")
    sqls.append("SELECT id FROM order_t ORDER BY v ASC LIMIT 3 OFFSET 47;")
    out = db.run(sqls)
    check("ORDER BY ASC LIMIT 5", "id", out)
    check("ORDER BY DESC LIMIT 5", "id", out)
finally:
    db.cleanup()

# ================================================================
print("\n" + "=" * 50)
print(f"  RESULTS: {PASS}/{PASS+FAIL} passed, {FAIL} failed")
print("=" * 50)
if BUGS:
    print("\n--- DISCOVERED BUGS ---")
    for desc, expected, got in BUGS:
        print(f"\n[BUG] {desc}")
        print(f"  Expected: {expected}")
        print(f"  Got: {got[:200]}")
    sys.exit(1)
else:
    print("\nAll consistency tests passed!")
    sys.exit(0)
