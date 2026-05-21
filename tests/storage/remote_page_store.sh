#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: remote_page_store.sh /path/to/minidb}"
PAGESERVER_BIN="$(dirname "$BIN")/minidb_pageserver"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-remote-db.XXXXXX")"
PAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-page-server.XXXXXX")"
PORT="$(python3 - <<'PY'
import socket
s=socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"
PS_LOG="$PAGE_DIR/pageserver.log"
PS_PID=""
cleanup() {
    if [[ -n "$PS_PID" ]]; then
        kill "$PS_PID" >/dev/null 2>&1 || true
        wait "$PS_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$DB_DIR" "$PAGE_DIR"
}
trap cleanup EXIT

cat >"$DB_DIR/minidb.conf" <<EOF
storage_mode=remote
page_server_dir=$PAGE_DIR
page_server_host=127.0.0.1
page_server_port=$PORT
page_server_replicas=1
remote_page_batch_size=8
remote_flush_batch_size=8
remote_connect_timeout=1s
remote_io_timeout=5s
remote_retry_count=2
shared_buffers=1MB
statement_timeout=10s
wal_group_commit=on
EOF

"$PAGESERVER_BIN" --dir "$PAGE_DIR" --host 127.0.0.1 --port "$PORT" --config "$DB_DIR/minidb.conf" >"$PS_LOG" 2>&1 &
PS_PID="$!"
for _ in $(seq 1 50); do
    if grep -q 'PageServer listening' "$PS_LOG"; then
        break
    fi
    sleep 0.1
done
if ! grep -q 'PageServer listening' "$PS_LOG"; then
    printf 'pageserver did not start\n' >&2
    cat "$PS_LOG" >&2
    exit 1
fi

run_sql() {
    printf '%s\n' "$@" 'exit' | "$BIN" --dir "$DB_DIR" --config "$DB_DIR/minidb.conf"
}

require_contains() {
    local needle="$1"
    local haystack="$2"
    if [[ "$haystack" != *"$needle"* ]]; then
        printf 'expected output to contain: %s\n' "$needle" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

cfg="$("$BIN" --dir "$DB_DIR" --config "$DB_DIR/minidb.conf" --show-config)"
require_contains 'storage_mode=remote' "$cfg"
require_contains "page_server_host=127.0.0.1" "$cfg"
require_contains 'page_server_replicas=1' "$cfg"

out1="$(
    run_sql \
        'CREATE TABLE remote_t (id INT PRIMARY KEY, v TEXT);' \
        'INSERT INTO remote_t VALUES (1, "one"), (2, "two"), (3, "three");' \
        'EXPLAIN SELECT count(*) FROM remote_t;' \
        'EXPLAIN ANALYZE SELECT count(*) FROM remote_t;' \
        'SELECT count(*) FROM remote_t;' \
        'SELECT v FROM remote_t WHERE id = 2;' \
        'SHOW STATS;'
)"

require_contains 'agg_0
3' "$out1"
require_contains 'remote-cost' "$out1"
require_contains 'Execution: actual_rows=1' "$out1"
require_contains 'v
two' "$out1"
require_contains 'storage_mode=remote' "$out1"
require_contains 'page_server_host=127.0.0.1' "$out1"
require_contains 'remote_read_batches=' "$out1"
require_contains 'remote_write_batches=' "$out1"

kill "$PS_PID" >/dev/null 2>&1 || true
wait "$PS_PID" >/dev/null 2>&1 || true
PS_PID=""
"$PAGESERVER_BIN" --dir "$PAGE_DIR" --host 127.0.0.1 --port "$PORT" --config "$DB_DIR/minidb.conf" >>"$PS_LOG" 2>&1 &
PS_PID="$!"
sleep 0.3

out2="$(
    run_sql \
        'SELECT count(*) FROM remote_t;' \
        'SELECT v FROM remote_t WHERE id = 2;'
)"

require_contains 'agg_0
3' "$out2"
require_contains 'v
two' "$out2"

if [[ ! -d "$PAGE_DIR/replica_1/tables" ]]; then
    printf 'expected page server replica directory to exist\n' >&2
    exit 1
fi

if [[ ! -s "$PAGE_DIR/remote_wal_images.bin" || ! -s "$PAGE_DIR/page_server.meta" ]]; then
    printf 'expected pageserver WAL image and metadata files to exist\n' >&2
    exit 1
fi
