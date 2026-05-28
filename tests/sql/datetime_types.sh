#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: datetime_types.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-datetime.XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

run_sql() {
    printf '%s\n' "$@" 'exit' | "$BIN" --dir "$DB_DIR"
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

out="$(
    run_sql \
        "CREATE TABLE events (id INT PRIMARY KEY, created_at TIMESTAMP NOT NULL, scheduled DATETIME DEFAULT '2026-05-25 21:00:00.5');" \
        "INSERT INTO events (id, created_at) VALUES (1, '2026-05-25 20:15:30');" \
        "INSERT INTO events VALUES (2, '2026-05-25T20:16:30.123456', '2026-05-26 00:00:00');" \
        "SELECT id, created_at, scheduled FROM events ORDER BY created_at;" \
        "SELECT id FROM events WHERE created_at BETWEEN '2026-05-25 20:15:00' AND '2026-05-25 20:16:00';" \
        "CREATE INDEX idx_events_created ON events (created_at);" \
        "EXPLAIN SELECT id FROM events WHERE created_at = '2026-05-25 20:15:30';" \
        "SELECT id FROM events WHERE created_at = '2026-05-25 20:15:30';" \
        "UPDATE events SET scheduled = CAST('2026-05-27 10:00:00.25' AS DATETIME) WHERE id = 1;" \
        "SELECT scheduled FROM events WHERE id = 1;" \
        "DESCRIBE events;"
)"

require_contains "2026-05-25 20:15:30" "$out"
require_contains "2026-05-25 20:16:30.123456" "$out"
require_contains "2026-05-25 21:00:00.5" "$out"
require_contains "IndexScan table=events" "$out"
require_contains "2026-05-27 10:00:00.25" "$out"
require_contains "TIMESTAMP" "$out"
require_contains "DATETIME" "$out"

printf 'datetime_types passed\n'
