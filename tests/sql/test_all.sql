-- ============================================================
-- MiniDB full-feature smoke script.
-- Usage: cat test_all.sql | ./minidb
-- ============================================================

-- 1. DDL: CREATE TABLE
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT, salary FLOAT);
SHOW TABLES;
DESC users;

-- 2. DML: INSERT
INSERT INTO users VALUES (1, 'Alice', 30, 50000.0);
INSERT INTO users VALUES (2, 'Bob', 25, 45000.0);
INSERT INTO users VALUES (3, 'Charlie', 35, 60000.0);
INSERT INTO users VALUES (4, 'David', 28, 52000.0);
INSERT INTO users VALUES (5, 'Eve', 32, 55000.0);

-- 3. SELECT: basic queries
SELECT * FROM users;
SELECT name, age FROM users;
SELECT * FROM users WHERE age > 28;

-- 4. SELECT: expression projection
SELECT name, age + 1 AS next_age FROM users;

-- 5. SELECT: ORDER BY + LIMIT
SELECT * FROM users ORDER BY age ASC LIMIT 3;
SELECT * FROM users ORDER BY salary DESC;

-- 6. SELECT: DISTINCT
SELECT DISTINCT age FROM users;

-- 7. SELECT: aggregate functions
SELECT COUNT(*), AVG(salary), MAX(age), MIN(age) FROM users;

-- 8. SELECT: GROUP BY + HAVING
SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) > 1;

-- 9. SELECT: BETWEEN
SELECT * FROM users WHERE age BETWEEN 28 AND 32;

-- 10. SELECT: LIKE
SELECT * FROM users WHERE name LIKE '%li%';

-- 11. SELECT: IN (value list)
SELECT * FROM users WHERE age IN (25, 30, 35);

-- 12. SELECT: CASE WHEN
SELECT name, CASE WHEN age >= 30 THEN 'senior' ELSE 'junior' END AS level FROM users;

-- 13. SELECT: COALESCE / NULLIF
SELECT COALESCE(name, 'unknown') FROM users;
SELECT NULLIF(age, 30) FROM users;

-- 14. UPDATE
UPDATE users SET salary = 65000 WHERE name = 'Alice';
SELECT * FROM users WHERE name = 'Alice';

-- 15. DELETE
DELETE FROM users WHERE name = 'Bob';
SELECT * FROM users;

-- 16. Transactions: BEGIN + ROLLBACK
BEGIN;
INSERT INTO users VALUES (6, 'Frank', 40, 70000.0);
SELECT * FROM users WHERE name = 'Frank';
ROLLBACK;
SELECT * FROM users WHERE name = 'Frank';

-- 17. Transactions: BEGIN + COMMIT
BEGIN;
INSERT INTO users VALUES (7, 'Grace', 27, 48000.0);
COMMIT;
SELECT * FROM users WHERE name = 'Grace';

-- 18. JOIN
CREATE TABLE orders (id INT, user_id INT, amount FLOAT);
INSERT INTO orders VALUES (1, 1, 100.0);
INSERT INTO orders VALUES (2, 1, 200.0);
INSERT INTO orders VALUES (3, 2, 150.0);
INSERT INTO orders VALUES (4, 3, 300.0);

SELECT u.name, o.amount FROM users u INNER JOIN orders o ON u.id = o.user_id;
SELECT u.name, o.amount FROM users u LEFT JOIN orders o ON u.id = o.user_id;

-- 19. UNION
SELECT name FROM users WHERE age < 30 UNION ALL SELECT name FROM users WHERE age > 32;

-- 20. EXPLAIN
EXPLAIN SELECT * FROM users WHERE age > 25;
EXPLAIN SELECT u.name, o.amount FROM users u INNER JOIN orders o ON u.id = o.user_id;

-- 21. Error handling.
SELECT * FROM nonexistent_table;
INSERT INTO users VALUES (1, 'Dup', 20, 1000.0);
SHOW DATABASES;

-- 22. Cleanup.
SHOW TABLES;
