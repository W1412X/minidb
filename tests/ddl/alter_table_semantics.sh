#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: alter_table_semantics.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-alter.XXXXXX")"
SQL1="$(mktemp "${TMPDIR:-/tmp}/minidb-alter-1.XXXXXX")"
SQL2="$(mktemp "${TMPDIR:-/tmp}/minidb-alter-2.XXXXXX")"
OUT1="$(mktemp "${TMPDIR:-/tmp}/minidb-alter-out1.XXXXXX")"
OUT2="$(mktemp "${TMPDIR:-/tmp}/minidb-alter-out2.XXXXXX")"
trap 'rm -rf "$DB_DIR"; rm -f "$SQL1" "$SQL2" "$OUT1" "$OUT2"' EXIT

cat > "$SQL1" <<'SQL'
CREATE TABLE alt (id INT PRIMARY KEY, name TEXT, v INT);
INSERT INTO alt VALUES (1, "alice", 10);
INSERT INTO alt VALUES (2, "bob", 20);
ALTER TABLE alt ADD COLUMN age INT DEFAULT 25 NOT NULL;
SELECT id, age FROM alt ORDER BY id;
INSERT INTO alt VALUES (3, "carl", 30, 35);
ALTER TABLE alt ADD COLUMN must_have INT NOT NULL;
SELECT COUNT(*) FROM alt;
ALTER TABLE alt RENAME COLUMN name TO full_name;
SELECT full_name FROM alt WHERE id = 1;
CREATE INDEX alt_v_idx ON alt(v);
ALTER TABLE alt DROP COLUMN v;
ALTER TABLE alt DROP COLUMN age;
SELECT id, full_name FROM alt ORDER BY id;
DESC alt;
exit
SQL

"$BIN" --dir "$DB_DIR" < "$SQL1" > "$OUT1"

if grep -qE 'failed to build plan|failed to create executor|unsupported or unrecognized' "$OUT1"; then
    cat "$OUT1" >&2
    exit 1
fi

grep -q '^1 | 25$' "$OUT1"
grep -q '^2 | 25$' "$OUT1"
grep -q '^alice$' "$OUT1"
grep -q 'cannot add NOT NULL column without DEFAULT' "$OUT1"
grep -q 'cannot drop indexed column' "$OUT1"
grep -q '^1 | alice$' "$OUT1"
grep -q '^3 | carl$' "$OUT1"
grep -q 'full_name' "$OUT1"
if grep -q ' age ' "$OUT1"; then
    printf 'expected dropped age column to disappear from DESC\n' >&2
    cat "$OUT1" >&2
    exit 1
fi

cat > "$SQL2" <<'SQL'
SELECT id, full_name FROM alt ORDER BY id;
DESC alt;
exit
SQL

"$BIN" --dir "$DB_DIR" < "$SQL2" > "$OUT2"

if grep -qE 'Error:|failed to build plan|failed to create executor|unsupported or unrecognized' "$OUT2"; then
    cat "$OUT2" >&2
    exit 1
fi

grep -q '^1 | alice$' "$OUT2"
grep -q '^2 | bob$' "$OUT2"
grep -q '^3 | carl$' "$OUT2"
grep -q 'full_name' "$OUT2"
if grep -q ' age ' "$OUT2"; then
    printf 'expected dropped age column to stay dropped after restart\n' >&2
    cat "$OUT2" >&2
    exit 1
fi
