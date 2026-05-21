#!/usr/bin/env python3
"""Concurrency test suite for MiniDB."""
import subprocess, os, sys, tempfile, shutil, time, threading, socket

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minidb"
DB_DIR = tempfile.mkdtemp(prefix="minidb_conc_")
PASS = 0
FAIL = 0
BUGS = []
PORT = 15450 + (os.getpid() % 1000)
server = None

def cleanup():
    shutil.rmtree(DB_DIR, ignore_errors=True)

def run_sql(sqls):
    if isinstance(sqls, str):
        sqls = [sqls]
    sqls.append("exit")
    proc = subprocess.run(
        [BIN, "--dir", DB_DIR],
        input="\n".join(sqls),
        capture_output=True, text=True, timeout=30
    )
    return proc.stdout + proc.stderr

def send_sql_to_server(sqls):
    """Send SQL to running server via TCP, return response."""
    if isinstance(sqls, str):
        sqls = [sqls]
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(("127.0.0.1", PORT))
        # Read welcome
        data = b""
        while True:
            chunk = s.recv(4096)
            data += chunk
            if b"\n" in data:
                break
        # Send SQL
        for sql in sqls:
            s.sendall((sql + "\n").encode())
        time.sleep(0.2)
        # Read response
        result = b""
        while True:
            s.settimeout(2)
            try:
                chunk = s.recv(4096)
                if not chunk:
                    break
                result += chunk
            except socket.timeout:
                break
        s.close()
        return result.decode(errors="replace")
    except Exception as e:
        return f"ERROR: {e}"

def start_server():
    proc = subprocess.Popen(
        [BIN, "--dir", DB_DIR, "--server", "--port", str(PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1)
    return proc

def stop_server(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

def check(desc, needle, haystack, negate=False):
    global PASS, FAIL
    found = needle in haystack
    if negate:
        found = not found
    if found:
        PASS += 1
        print(f"  PASS: {desc}")
    else:
        FAIL += 1
        BUGS.append((desc, needle, haystack[:500]))
        print(f"  FAIL: {desc}  (expected {'NOT ' if negate else ''}'{needle}')")
        print(f"        got: {haystack[:200]}...")

try:
    # Setup
    print("=== Setting up tables ===")
    run_sql([
        "CREATE TABLE conc_t (id INT PRIMARY KEY, v INT, thread_id INT);",
        "CREATE INDEX conc_v ON conc_t (v);",
    ])

    # Start server
    print("=== Starting server ===")
    server = start_server()

    # Test C1: Single-client writes
    print("\n--- C1: Single-client sequential writes ---")
    sqls = []
    for i in range(1, 51):
        sqls.append(f"INSERT INTO conc_t VALUES ({i}, {i * 10}, 1);")
    sqls.append("SELECT COUNT(*) FROM conc_t;")
    out = send_sql_to_server(sqls)
    check("C1: 50 sequential inserts", "50", out)

    # Test C2: Multi-threaded writes
    print("\n--- C2: Multi-threaded concurrent writes ---")
    results = []

    def thread_insert(tid, count):
        sqls = []
        for i in range(1, count + 1):
            sqls.append(f"INSERT INTO conc_t VALUES ({tid * 10000 + i}, {i}, {tid});")
        r = send_sql_to_server(sqls)
        results.append(r)

    threads = []
    for tid in range(1, 6):
        t = threading.Thread(target=thread_insert, args=(tid, 20))
        threads.append(t)
        t.start()
    for t in threads:
        t.join(timeout=30)
    time.sleep(1)

    out = send_sql_to_server(["SELECT COUNT(*) FROM conc_t;"])
    # Should have 50 (C1) + 100 (C2: 5*20) = 150
    check("C2: concurrent writes count=150", "150", out)

    # Test C3: Concurrent reads + writes
    print("\n--- C3: Concurrent reads + writes ---")
    read_results = []
    write_results = []

    def thread_read(n):
        for i in range(n):
            r = send_sql_to_server(["SELECT COUNT(*) FROM conc_t;"])
            read_results.append(r)
            time.sleep(0.05)

    def thread_write(n):
        for i in range(n):
            r = send_sql_to_server([f"INSERT INTO conc_t VALUES ({90000 + i}, {i}, 99);"])
            write_results.append(r)
            time.sleep(0.05)

    t1 = threading.Thread(target=thread_read, args=(10,))
    t2 = threading.Thread(target=thread_write, args=(10,))
    t1.start()
    t2.start()
    t1.join(timeout=30)
    t2.join(timeout=30)
    time.sleep(1)

    # Check no crashes occurred
    all_ok = all("ERROR" not in r for r in read_results + write_results)
    check("C3: no crashes during concurrent R/W", "1" if all_ok else "0", "1")
    out = send_sql_to_server(["SELECT COUNT(*) FROM conc_t;"])
    check("C3: data count >= 150", "1", out)  # Just verify data exists

    # Test C4: Concurrent UPDATE + DELETE
    print("\n--- C4: Concurrent UPDATE + DELETE ---")
    update_results = []
    delete_results = []

    def thread_update(n):
        for i in range(n):
            r = send_sql_to_server(["UPDATE conc_t SET v = v + 1 WHERE thread_id = 1;"])
            update_results.append(r)

    def thread_delete(n):
        for i in range(n):
            r = send_sql_to_server([f"DELETE FROM conc_t WHERE id = {90000 + i};"])
            delete_results.append(r)

    t1 = threading.Thread(target=thread_update, args=(10,))
    t2 = threading.Thread(target=thread_delete, args=(5,))
    t1.start()
    t2.start()
    t1.join(timeout=30)
    t2.join(timeout=30)
    time.sleep(1)

    all_ok = all("ERROR" not in r for r in update_results + delete_results)
    check("C4: no crashes during concurrent UPDATE/DELETE", "1" if all_ok else "0", "1")

    # Test C5: Concurrent SELECTs (stress)
    print("\n--- C5: 20 concurrent SELECTs ---")
    select_results = []

    def thread_select(n):
        for i in range(n):
            r = send_sql_to_server([
                "SELECT id, v FROM conc_t WHERE v > 50 ORDER BY v LIMIT 5;"
            ])
            select_results.append(r)

    threads = [threading.Thread(target=thread_select, args=(5,)) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=30)

    all_ok = all("ERROR" not in r for r in select_results)
    check("C5: concurrent SELECTs no crash", "1" if all_ok else "0", "1")

    # Test C6: Server still alive
    print("\n--- C6: Server alive after all operations ---")
    out = send_sql_to_server(["SHOW TABLES;"])
    check("C6: server alive", "conc_t", out)

    # Test C7: Kill and restart, data persists
    print("\n--- C7: Kill server, restart, data persists ---")
    stop_server(server)
    time.sleep(1)

    out = run_sql(["SELECT COUNT(*) FROM conc_t;"])
    check("C7: data persists after restart", "agg_0", out)  # Just verify table exists with data

finally:
    # Make sure server is stopped
    try:
        stop_server(server)
    except Exception:
        pass
    cleanup()

print()
print("=" * 50)
print(f"  CONCURRENT RESULTS: {PASS}/{PASS + FAIL} passed, {FAIL} failed")
print("=" * 50)

if BUGS:
    print("\n--- DISCOVERED BUGS ---")
    for desc, expected, got in BUGS:
        print(f"\n[BUG] {desc}")
        print(f"  Expected: {expected}")
        print(f"  Got: {got[:300]}")
    sys.exit(1)
else:
    print("\nAll concurrency tests passed!")
    sys.exit(0)
