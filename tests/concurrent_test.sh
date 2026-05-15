#!/usr/bin/env bash
set -uo pipefail

BIN="${1:?usage: concurrent_test.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-conc.XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

PASS=0
FAIL=0
TOTAL=0
BUGS=""

run_sql() {
    printf '%s\n' "$@" 'exit' | "$BIN" --dir "$DB_DIR" 2>&1
}

send_sql() {
    local port="$1"
    local payload
    payload="$(cat)"
    PAYLOAD="$payload" python3 - "$port" <<'PY'
import os
import socket
import sys

port = int(sys.argv[1])
payload = os.environ.get("PAYLOAD", "")
with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
    sock.settimeout(5)
    try:
        sock.recv(4096)  # welcome banner
    except socket.timeout:
        pass
    sock.sendall(payload.encode())
    sock.shutdown(socket.SHUT_WR)
    chunks = []
    while True:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            break
        if not data:
            break
        chunks.append(data)
sys.stdout.write(b"".join(chunks).decode(errors="replace"))
PY
}

check() {
    local desc="$1"
    local needle="$2"
    local haystack="$3"
    TOTAL=$((TOTAL+1))
    if [[ "$haystack" == *"$needle"* ]]; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        BUGS="${BUGS}\n[BUG] $desc\n  Expected: $needle\n  Got (first 500 chars): ${haystack:0:500}"
    fi
}

echo "=== Concurrent Test Suite ==="

# Setup tables
run_sql \
    "CREATE TABLE conc_t (id INT PRIMARY KEY, v INT, thread_id INT);" \
    "CREATE INDEX conc_v ON conc_t (v);" \
    "SHOW TABLES;" > /dev/null

echo "--- Test C1: Concurrent INSERT via server mode ---"
# Start server in background
TMPDIR_SERVER="$(mktemp -d)"
"$BIN" --dir "$DB_DIR" --server --port 15432 &
SERVER_PID=$!
sleep 1

# Send concurrent writes via separate nc connections
for tid in $(seq 1 5); do
    for i in $(seq 1 20); do
        echo "INSERT INTO conc_t VALUES ($((tid * 1000 + i)), $((i * 10)), $tid);"
    done
done | send_sql 15432 > /dev/null 2>&1 &
CONC_PID=$!
wait $CONC_PID 2>/dev/null

sleep 1
# Verify data
out="$(echo "SELECT COUNT(*) FROM conc_t;" | send_sql 15432 2>&1)"
check "Concurrent inserts: count > 0" "100" "$out"
# Should have 100 rows (5 threads * 20 inserts)
check "Concurrent inserts: count = 100" "100" "$out"

echo "--- Test C2: Concurrent Read + Write via server ---"
(
    for i in $(seq 1 10); do
        echo "SELECT COUNT(*) FROM conc_t;"
        sleep 0.1
    done
) | send_sql 15432 > /dev/null 2>&1 &
READ_PID=$!

(
    for i in $(seq 1 10); do
        echo "INSERT INTO conc_t VALUES ($((10000 + i)), $((i * 100)), 99);"
        sleep 0.1
    done
) | send_sql 15432 > /dev/null 2>&1 &
WRITE_PID=$!

wait $READ_PID 2>/dev/null
wait $WRITE_PID 2>/dev/null
sleep 1

out="$(echo "SELECT COUNT(*) FROM conc_t;" | send_sql 15432 2>&1)"
# Should have at least 100 original rows
check "Concurrent read/write: count >= 100" "110" "$out"

echo "--- Test C3: Concurrent UPDATE + DELETE via server ---"
(
    for i in $(seq 1 10); do
        echo "UPDATE conc_t SET v = v + 1 WHERE thread_id = 1;"
        sleep 0.05
    done
) | send_sql 15432 > /dev/null 2>&1 &
UPDATE_PID=$!

(
    for i in $(seq 1 5); do
        echo "DELETE FROM conc_t WHERE id = $((1000 + i));"
        sleep 0.05
    done
) | send_sql 15432 > /dev/null 2>&1 &
DELETE_PID=$!

wait $UPDATE_PID 2>/dev/null
wait $DELETE_PID 2>/dev/null
sleep 1

out="$(echo "SELECT COUNT(*) FROM conc_t;" | send_sql 15432 2>&1)"
check "Concurrent update/delete: still has data" "105" "$out"

echo "--- Test C4: Concurrent SELECT (no crash) ---"
(
    for i in $(seq 1 20); do
        echo "SELECT id, v FROM conc_t WHERE v > 50 ORDER BY v LIMIT 5;"
        sleep 0.02
    done
) | send_sql 15432 > /dev/null 2>&1 &
SELECT_PID=$!
wait $SELECT_PID 2>/dev/null
check "Concurrent SELECTs: no crash" "ok" "ok"

echo "--- Test C5: Server still alive after all operations ---"
out="$(echo "SHOW TABLES;" | send_sql 15432 2>&1)"
check "Server alive after concurrency: shows tables" "conc_t" "$out"

echo "--- Test C6: Kill server cleanly ---"
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
sleep 0.5
check "Server killed cleanly" "ok" "ok"

echo "--- Test C7: Data persists after server restart ---"
"$BIN" --dir "$DB_DIR" --server --port 15433 &
SERVER_PID2=$!
sleep 1

out="$(echo "SELECT COUNT(*) FROM conc_t;" | send_sql 15433 2>&1)"
check "Data persists after restart: count > 0" "105" "$out"

kill $SERVER_PID2 2>/dev/null
wait $SERVER_PID2 2>/dev/null

echo ""
echo "========================================="
echo "  CONCURRENT TEST RESULTS: $PASS/$TOTAL passed, $FAIL failed"
echo "========================================="
if [ $FAIL -gt 0 ]; then
    echo -e "\n--- DISCOVERED BUGS ---$BUGS"
fi
echo ""
