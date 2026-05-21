#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: wal_replay_slot_reuse.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-wal-slot-reuse.XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

run_sql() {
    printf '%s\n' "$@" 'exit' | "$BIN" --dir "$DB_DIR"
}

setup=(
    'CREATE TABLE kv (id INT PRIMARY KEY, v INT);'
    'CREATE INDEX idx_kv_v ON kv(v);'
)
for i in $(seq 1 50); do
    setup+=("INSERT INTO kv VALUES ($i, $((i * 10)));")
done
run_sql "${setup[@]}" >/dev/null

# Each statement uses a fresh process to force startup WAL recovery. The old
# DELETE records below free slots that the later UPDATE can reuse; recovery must
# not replay those old DELETEs over the newer tuple occupying the same slot.
run_sql 'DELETE FROM kv WHERE id = 13;' >/dev/null
run_sql 'DELETE FROM kv WHERE id = 15;' >/dev/null
run_sql 'UPDATE kv SET v = 439 WHERE id = 20;' >/dev/null

out="$(run_sql \
    'SELECT id, v FROM kv WHERE id = 20;' \
    'SELECT id, v FROM kv ORDER BY id LIMIT 20;')"

if [[ "$out" != *$'20 | 439'* ]]; then
    printf 'expected updated row in reused slot to survive WAL replay\n' >&2
    printf '%s\n' "$out" >&2
    exit 1
fi

if [[ "$out" == *$'13 | 130'* || "$out" == *$'15 | 150'* ]]; then
    printf 'expected deleted rows to remain deleted after WAL replay\n' >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
