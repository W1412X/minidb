#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: recovery_smoke.sh /path/to/minidb}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/minidb-recovery.XXXXXX")"
OUT1="$(mktemp "${TMPDIR:-/tmp}/minidb-recovery-out.XXXXXX")"
FIFO1="$(mktemp -u "${TMPDIR:-/tmp}/minidb-recovery-fifo1.XXXXXX")"
FIFO2="$(mktemp -u "${TMPDIR:-/tmp}/minidb-recovery-fifo2.XXXXXX")"
FIFO3="$(mktemp -u "${TMPDIR:-/tmp}/minidb-recovery-fifo3.XXXXXX")"
trap 'rm -rf "$DB_DIR" "$OUT1" "$FIFO1" "$FIFO2" "$FIFO3"' EXIT

mkfifo "$FIFO1"
"$BIN" --dir "$DB_DIR" <"$FIFO1" >"$OUT1" 2>&1 &
DBPROC_PID=$!
exec 3>"$FIFO1"
sleep 0.2
printf 'CREATE TABLE wal_t (id INT PRIMARY KEY, v TEXT);\n' >&3
sleep 0.1
printf 'BEGIN;\n' >&3
sleep 0.1
printf 'INSERT INTO wal_t VALUES (1, "redo");\n' >&3
sleep 0.1
printf 'COMMIT;\n' >&3
sleep 0.3
kill -9 "$DBPROC_PID" >/dev/null 2>&1 || true
exec 3>&-
wait "$DBPROC_PID" >/dev/null 2>&1 || true

out2="$(
    printf '%s\n' \
        'SELECT id, v FROM wal_t WHERE id = 1;' \
        'exit' | "$BIN" --dir "$DB_DIR"
)"

if [[ "$out2" != *$'1 | redo'* ]]; then
    printf 'expected WAL recovery to restore committed row\n' >&2
    printf '%s\n' "$out2" >&2
    exit 1
fi

mkfifo "$FIFO2"
"$BIN" --dir "$DB_DIR" <"$FIFO2" >/dev/null 2>&1 &
DBPROC2_PID=$!
exec 4>"$FIFO2"
sleep 0.2
printf 'BEGIN;\n' >&4
sleep 0.1
printf 'INSERT INTO wal_t VALUES (2, "uncommitted");\n' >&4
sleep 0.2
kill -9 "$DBPROC2_PID" >/dev/null 2>&1 || true
exec 4>&-
wait "$DBPROC2_PID" >/dev/null 2>&1 || true

out3="$(
    printf '%s\n' \
        'SELECT id, v FROM wal_t WHERE id = 2;' \
        'exit' | "$BIN" --dir "$DB_DIR"
)"

if [[ "$out3" == *'uncommitted'* ]]; then
    printf 'expected crash recovery to hide uncommitted row\n' >&2
    printf '%s\n' "$out3" >&2
    exit 1
fi

mkfifo "$FIFO3"
"$BIN" --dir "$DB_DIR" <"$FIFO3" >/dev/null 2>&1 &
DBPROC3_PID=$!
exec 5>"$FIFO3"
sleep 0.2
printf 'CREATE TABLE idxr (id INT PRIMARY KEY, v INT, note TEXT);\n' >&5
sleep 0.1
printf 'CREATE INDEX idxr_v_idx ON idxr(v);\n' >&5
sleep 0.1
printf 'INSERT INTO idxr VALUES (1, 10, "one");\n' >&5
sleep 0.1
printf 'INSERT INTO idxr VALUES (2, 20, "two");\n' >&5
sleep 0.1
printf 'UPDATE idxr SET id = 10, v = 11 WHERE id = 1;\n' >&5
sleep 0.1
printf 'DELETE FROM idxr WHERE id = 2;\n' >&5
sleep 0.3
kill -9 "$DBPROC3_PID" >/dev/null 2>&1 || true
exec 5>&-
wait "$DBPROC3_PID" >/dev/null 2>&1 || true

out4="$(
    printf '%s\n' \
        'EXPLAIN SELECT note FROM idxr WHERE id = 10;' \
        'SELECT id, v, note FROM idxr WHERE id = 10;' \
        'SELECT COUNT(*) FROM idxr WHERE id = 1;' \
        'SELECT COUNT(*) FROM idxr WHERE id = 2;' \
        'SELECT COUNT(*) FROM idxr WHERE v = 11;' \
        'exit' | "$BIN" --dir "$DB_DIR"
)"

if [[ "$out4" != *'IndexScan table=idxr'* ||
      "$out4" != *$'10 | 11 | one'* ||
      "$out4" != *$'agg_0\n0'* ||
      "$out4" != *$'agg_0\n1'* ]]; then
    printf 'expected WAL recovery plus lazy index rebuild to restore index consistency\n' >&2
    printf '%s\n' "$out4" >&2
    exit 1
fi
