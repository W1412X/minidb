-- ============================================================
-- MiniDB full-feature exercise.
-- Covers: HOT Update, optimizer, lock manager, shared memory.
-- ============================================================

-- 1. Basic DDL
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT, salary FLOAT);
CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, amount FLOAT);
CREATE INDEX idx_user_id ON orders (user_id);
SHOW TABLES;

-- 2. INSERT
INSERT INTO users VALUES (1, 'Alice', 30, 50000.0);
INSERT INTO users VALUES (2, 'Bob', 25, 45000.0);
INSERT INTO users VALUES (3, 'Charlie', 35, 60000.0);
INSERT INTO orders VALUES (1, 1, 100.0);
INSERT INTO orders VALUES (2, 1, 200.0);
INSERT INTO orders VALUES (3, 2, 150.0);

-- 3. HOT update: modify a non-indexed column (name is not indexed).
--    new version stays on the same page, indexes untouched.
UPDATE users SET name = 'Alice2' WHERE id = 1;
SELECT * FROM users WHERE id = 1;

-- 4. Non-HOT update: modify the primary key column (id is indexed).
--    inserts on a new page and updates the index.
UPDATE users SET age = 31 WHERE id = 1;
SELECT * FROM users WHERE id = 1;

-- 5. Optimizer: index scan vs full table scan.
EXPLAIN SELECT * FROM orders WHERE user_id = 1;
SELECT * FROM orders WHERE user_id = 1;

-- 6. Optimizer: range query.
EXPLAIN SELECT * FROM users WHERE age > 28;
SELECT * FROM users WHERE age > 28;

-- 7. JOIN + Optimizer
EXPLAIN SELECT u.name, o.amount FROM users u INNER JOIN orders o ON u.id = o.user_id;
SELECT u.name, o.amount FROM users u INNER JOIN orders o ON u.id = o.user_id;

-- 8. Aggregation.
SELECT COUNT(*), AVG(salary) FROM users;

-- 9. GROUP BY + HAVING
SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) > 1;

-- 10. ORDER BY + LIMIT
SELECT * FROM users ORDER BY salary DESC LIMIT 2;

-- 11. DISTINCT
SELECT DISTINCT age FROM users;

-- 12. BETWEEN
SELECT * FROM users WHERE age BETWEEN 25 AND 32;

-- 13. LIKE
SELECT * FROM users WHERE name LIKE '%li%';

-- 14. IN
SELECT * FROM users WHERE age IN (25, 30, 35);

-- 15. CASE WHEN
SELECT name, CASE WHEN age >= 30 THEN 'senior' ELSE 'junior' END AS level FROM users;

-- 16. COALESCE / NULLIF
SELECT COALESCE(name, 'unknown') FROM users;
SELECT NULLIF(age, 30) FROM users;

-- 17. DELETE
DELETE FROM users WHERE id = 2;
SELECT * FROM users;

-- 18. Transaction ROLLBACK.
BEGIN;
INSERT INTO users VALUES (10, 'Temp', 99, 99999.0);
SELECT * FROM users WHERE id = 10;
ROLLBACK;
SELECT * FROM users WHERE id = 10;

-- 19. Transaction COMMIT.
BEGIN;
INSERT INTO users VALUES (20, 'Committed', 40, 70000.0);
COMMIT;
SELECT * FROM users WHERE id = 20;

-- 20. LEFT JOIN
SELECT u.name, o.amount FROM users u LEFT JOIN orders o ON u.id = o.user_id;

-- 21. UNION ALL
SELECT name FROM users WHERE age < 30 UNION ALL SELECT name FROM users WHERE age > 35;

-- 22. Subquery IN.
SELECT * FROM users WHERE id IN (SELECT user_id FROM orders WHERE amount > 100);

-- 23. EXPLAIN on a complex query.
EXPLAIN SELECT u.name, SUM(o.amount) FROM users u INNER JOIN orders o ON u.id = o.user_id GROUP BY u.name;

-- 24. Final state.
SHOW TABLES;
