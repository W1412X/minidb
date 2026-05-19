#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?Usage: $0 ./build/minidb}"
DB_DIR="$(mktemp -d /tmp/minidb-keyword-combos.XXXXXX)"
OUT_FILE="$(mktemp /tmp/minidb-keyword-combos-out.XXXXXX)"
trap 'rm -rf "$DB_DIR"; rm -f "$OUT_FILE"' EXIT

"$BIN" --dir "$DB_DIR" > "$OUT_FILE" 2>&1 <<'SQL'
CREATE TABLE order (id INT PRIMARY KEY, count INT, sum INT, default INT, group INT, LENGTH INT);
CREATE TABLE order_items (id INT PRIMARY KEY, order_id INT, quantity INT, unit_price FLOAT);
INSERT INTO order (id, count, sum, default, group, LENGTH) VALUES (1, 10, 100, 7, 1, 5), (2, 20, 200, 8, 1, 6), (3, 30, 300, 9, 2, 7);
INSERT INTO order_items VALUES (1, 1, 2, 12.5), (2, 1, 3, 9.5), (11, 2, 4, 8.0);

SELECT count FROM order ORDER BY count LIMIT 1;
SELECT sum + default FROM order WHERE group = 1 ORDER BY id LIMIT 1;
SELECT LENGTH FROM order WHERE id = 1;
SELECT COUNT(*) FROM order CROSS JOIN order_items oi WHERE oi.id < 10;
SELECT COUNT(*) FROM order o INNER JOIN order_items oi ON o.id = oi.order_id WHERE oi.id < 10;
SELECT COUNT(*) FROM order o LEFT JOIN order_items oi ON o.id = oi.order_id WHERE oi.id < 10;
SELECT group, COUNT(*) AS count FROM order GROUP BY group ORDER BY group;
SELECT o.id AS order, oi.quantity AS group FROM order AS o INNER JOIN order_items AS oi ON o.id = oi.order_id ORDER BY order LIMIT 2;

CREATE INDEX idx_order_group ON order (group);
SELECT id FROM order WHERE group = 1 ORDER BY id;
UPDATE order SET default = default + 1 WHERE id = 1;
SELECT default FROM order WHERE id = 1;
DELETE FROM order WHERE id = 3;
SELECT COUNT(*) FROM order;
DESCRIBE order;

CREATE TABLE `select` (`from` INT PRIMARY KEY, `where` INT, `count` INT);
INSERT INTO `select` (`from`, `where`, `count`) VALUES (1, 2, 3);
SELECT `from`, `where`, `count` FROM `select`;
CREATE TABLE `left` (`join` INT PRIMARY KEY, `order` INT);
INSERT INTO `left` (`join`, `order`) VALUES (1, 9);
SELECT `join`, `order` FROM `left`;

CREATE TABLE default (to INT PRIMARY KEY, execute INT);
INSERT INTO default VALUES (1, 2);
SELECT to, execute FROM default;
DROP TABLE default;

PREPARE p AS SELECT count FROM order WHERE id = 1;
EXECUTE p;
DEALLOCATE p;
exit
SQL

if grep -qE 'Error: failed to build plan|Error: failed to create executor|unsupported or unrecognized|unexpected token|unexpected keyword' "$OUT_FILE"; then
    cat "$OUT_FILE" >&2
    exit 1
fi

grep -q '^10$' "$OUT_FILE"
grep -q '^107$' "$OUT_FILE"
grep -q '^5$' "$OUT_FILE"
grep -q '^6$' "$OUT_FILE"
grep -q '^2$' "$OUT_FILE"
grep -q '^1 | 2$' "$OUT_FILE"
grep -q '^8$' "$OUT_FILE"
grep -q 'from | where | count' "$OUT_FILE"
grep -q '^1 | 9$' "$OUT_FILE"
grep -q 'PREPARE' "$OUT_FILE"
grep -q 'DEALLOCATE' "$OUT_FILE"
