#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: full_sql_suite.sh /path/to/minidb}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-full-suite.XXXXXX")"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/minidb-full-suite-output.XXXXXX")"
trap 'rm -rf "$DB_DIR"; rm -f "$OUT_FILE"' EXIT

SUITE_SQL="$SCRIPT_DIR/test_suite.sql"

"$BIN" --dir "$DB_DIR" < "$SUITE_SQL" > "$OUT_FILE"

unsupported=$(grep -c 'unsupported or unrecognized' "$OUT_FILE" || true)
failed_plan=$(grep -c 'failed to build plan' "$OUT_FILE" || true)
failed_exec=$(grep -c 'failed to create executor' "$OUT_FILE" || true)

# Section 20 deliberately contains invalid SQL / unsupported commands.
if [[ "$unsupported" != "4" || "$failed_plan" != "2" || "$failed_exec" != "0" ]]; then
    printf 'unexpected suite error counts: unsupported=%s failed_plan=%s failed_exec=%s\n' \
        "$unsupported" "$failed_plan" "$failed_exec" >&2
    grep -nE 'unsupported or unrecognized|failed to build plan|failed to create executor|^minidb> Error:' \
        "$OUT_FILE" >&2 || true
    exit 1
fi
if grep -q 'unexpected token' "$OUT_FILE"; then
    printf 'parser artifact leaked into SQL output\n' >&2
    grep -n 'unexpected token' "$OUT_FILE" >&2
    exit 1
fi

grep -q "Table 'drop_test' dropped." "$OUT_FILE"
grep -q "Index 'idx_t2_a' created." "$OUT_FILE"
grep -q '3000000000' "$OUT_FILE"
grep -q 'high_earners' "$OUT_FILE"
