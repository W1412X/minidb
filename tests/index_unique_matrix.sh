#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: index_unique_matrix.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-uniq.XXXXXX")"
SQL1="$(mktemp "${TMPDIR:-/tmp}/minidb-uniq-1.XXXXXX")"
SQL2="$(mktemp "${TMPDIR:-/tmp}/minidb-uniq-2.XXXXXX")"
OUT1="$(mktemp "${TMPDIR:-/tmp}/minidb-uniq-out1.XXXXXX")"
OUT2="$(mktemp "${TMPDIR:-/tmp}/minidb-uniq-out2.XXXXXX")"
trap 'rm -rf "$DB_DIR"; rm -f "$SQL1" "$SQL2" "$OUT1" "$OUT2"' EXIT

cat > "$SQL1" <<'SQL'
CREATE TABLE u (id INT PRIMARY KEY, email TEXT UNIQUE, a INT, b INT, note TEXT);
CREATE UNIQUE INDEX ux_ab ON u(a, b);
INSERT INTO u VALUES (1, "a@example", 1, 1, "one");
INSERT INTO u VALUES (2, "b@example", 1, 2, "two");
INSERT INTO u VALUES (3, NULL, 2, 1, "null-one");
INSERT INTO u VALUES (4, NULL, 2, 2, "null-two");
INSERT INTO u VALUES (5, "a@example", 9, 9, "dup-email");
INSERT INTO u VALUES (6, "c@example", 1, 1, "dup-composite");
UPDATE u SET email = "a@example" WHERE id = 2;
UPDATE u SET a = 1, b = 1 WHERE id = 4;
SELECT COUNT(*) FROM u;
SELECT COUNT(*) FROM u WHERE email IS NULL;
SELECT id FROM u WHERE email = "a@example";
SELECT id FROM u WHERE a = 1 AND b = 1;
BEGIN;
INSERT INTO u VALUES (7, "rollback@example", 7, 7, "rollback");
ROLLBACK;
INSERT INTO u VALUES (7, "rollback@example", 7, 7, "after-rollback");
SELECT COUNT(*) FROM u WHERE id = 7;
SELECT id FROM u WHERE id = 7;
DELETE FROM u WHERE id = 2;
INSERT INTO u VALUES (8, "b@example", 8, 8, "reuse-after-delete");
SELECT COUNT(*) FROM u WHERE email = "b@example";
SELECT id FROM u WHERE email = "b@example";
exit
SQL

"$BIN" --dir "$DB_DIR" < "$SQL1" > "$OUT1"

if grep -qE 'Error:|failed to build plan|failed to create executor|unsupported or unrecognized' "$OUT1"; then
    cat "$OUT1" >&2
    exit 1
fi

grep -q $'agg_0\n4' "$OUT1"
grep -q $'agg_0\n2' "$OUT1"
grep -q '^1$' "$OUT1"
grep -q '^7$' "$OUT1"
grep -q '^8$' "$OUT1"

cat > "$SQL2" <<'SQL'
INSERT INTO u VALUES (9, "a@example", 9, 9, "dup-after-restart");
INSERT INTO u VALUES (9, "z@example", 9, 10, "dup-pk-after-restart");
INSERT INTO u VALUES (10, "z@example", 1, 1, "dup-composite-after-restart");
SELECT COUNT(*) FROM u;
SELECT COUNT(*) FROM u WHERE email = "a@example";
SELECT COUNT(*) FROM u WHERE id = 9;
SELECT COUNT(*) FROM u WHERE a = 1 AND b = 1;
exit
SQL

"$BIN" --dir "$DB_DIR" < "$SQL2" > "$OUT2"

if grep -qE 'Error:|failed to build plan|failed to create executor|unsupported or unrecognized' "$OUT2"; then
    cat "$OUT2" >&2
    exit 1
fi

grep -q $'agg_0\n5' "$OUT2"
if [[ "$(grep -c '^1$' "$OUT2")" -lt 3 ]]; then
    printf 'expected restart uniqueness checks to preserve single visible key rows\n' >&2
    cat "$OUT1" >&2
    cat "$OUT2" >&2
    exit 1
fi
