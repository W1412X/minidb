#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: semantic_validation.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-semantic.XXXXXX")"
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

require_not_contains() {
    local needle="$1"
    local haystack="$2"
    if [[ "$haystack" == *"$needle"* ]]; then
        printf 'expected output not to contain: %s\n' "$needle" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

expect_error() {
    local sql="$1"
    local out
    out="$(run_sql "$sql")"
    require_contains 'Error:' "$out"
    require_not_contains 'affected_rows' "$out"
}

run_sql \
    'CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT);' \
    'CREATE TABLE order_items (id INT PRIMARY KEY, order_id INT, qty INT);' \
    'INSERT INTO orders VALUES (1, 10), (2, 20);' \
    'INSERT INTO order_items VALUES (1, 1, 2), (2, 1, 3), (3, 2, 4);' >/dev/null

# Boolean contexts must be statically BOOL, never integer truthy/falsy.
expect_error 'SELECT COUNT(*) FROM order_items WHERE id;'
expect_error 'SELECT COUNT(*) FROM order_items WHERE id = 1 AND id;'
expect_error 'SELECT COUNT(*) FROM order_items WHERE NOT id;'
expect_error 'SELECT CASE WHEN id THEN 1 ELSE 0 END FROM order_items;'
expect_error 'SELECT COUNT(*) FROM order_items HAVING COUNT(*);'
expect_error 'SELECT COUNT(*) FROM order_items oi JOIN orders o ON oi.order_id;'
expect_error 'SELECT 1 HAVING 1;'
expect_error 'SELECT 1 FROM order_items HAVING id > 1;'
expect_error 'SELECT COUNT(*) FROM order_items HAVING id > 1;'
expect_error 'SELECT id + 1, COUNT(*) FROM order_items;'

# Qualified names must bind to a visible table name or alias.
expect_error 'SELECT COUNT(*) FROM order_items oi WHERE o.id < 10000;'
expect_error 'SELECT COUNT(*) FROM order_items WHERE o.id < 10000 AND o.id;'
expect_error 'SELECT id FROM orders o JOIN order_items oi ON o.id = oi.order_id WHERE id = 1;'

# DML predicates use the same BOOL rule.
expect_error 'UPDATE order_items SET qty = qty + 1 WHERE id;'
expect_error 'DELETE FROM order_items WHERE qty;'

valid_out="$(
    run_sql \
        'SELECT COUNT(*) FROM order_items oi WHERE oi.id < 10000 AND oi.qty > 0;' \
        'SELECT CASE WHEN id < 2 THEN 1 ELSE 0 END FROM order_items WHERE id = 1;' \
        'SELECT COUNT(*) FROM order_items HAVING COUNT(*) > 0;' \
        'SELECT 1 HAVING TRUE;' \
        'SELECT COUNT(*) + 1 FROM order_items;' \
        'SELECT order_id + 1, COUNT(*) FROM order_items GROUP BY order_id ORDER BY expr_0;' \
        'SELECT COUNT(*) FROM order_items oi JOIN orders o ON oi.order_id = o.id;' \
        'UPDATE order_items SET qty = qty + 1 WHERE id = 1;' \
        'DELETE FROM order_items WHERE id = 3;'
)"

require_contains 'agg_0
3' "$valid_out"
require_contains 'expr_0
1' "$valid_out"
require_contains 'expr_0
4' "$valid_out"
require_contains 'expr_0 | agg_0
2 | 2
3 | 1' "$valid_out"
require_contains 'affected_rows
1' "$valid_out"
require_contains 'deleted_rows
1' "$valid_out"
