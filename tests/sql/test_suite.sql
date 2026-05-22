-- 1.1 Basic CREATE TABLE.
CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR);
CREATE TABLE t2 (a INT, b INT, c FLOAT);
CREATE TABLE t3 (x BOOL, y BIGINT, z DOUBLE);
CREATE TABLE t4 (id INT PRIMARY KEY, val VARCHAR NOT NULL);
CREATE TABLE t5 (a INT PRIMARY KEY, b INT UNIQUE, c VARCHAR);
-- 1.2 Multi-column types.
CREATE TABLE all_types (c1 BOOL, c2 INT, c3 BIGINT, c4 FLOAT, c5 DOUBLE, c6 VARCHAR);
CREATE TABLE nullable_test (id INT, a INT, b VARCHAR);
CREATE TABLE pk_test (id INT PRIMARY KEY, data VARCHAR);
CREATE TABLE unique_test (id INT, email VARCHAR UNIQUE);
CREATE TABLE multi_col (a INT, b INT, c INT, d VARCHAR, e FLOAT);
-- 1.3 Show table structure.
SHOW TABLES;
DESC t1;
DESC t2;
DESC t3;
DESC t4;
DESC t5;
DESC all_types;
-- 1.4 CREATE INDEX.
CREATE INDEX idx_t2_a ON t2 (a);
CREATE INDEX idx_t2_b ON t2 (b);
CREATE UNIQUE INDEX idx_t4_val ON t4 (val);
CREATE INDEX idx_multi_a ON multi_col (a);
-- 1.5 DROP TABLE.
CREATE TABLE drop_test (id INT);
DROP TABLE drop_test;
SHOW TABLES;
-- 2.1 Basic INSERT.
INSERT INTO t1 VALUES (1, 'Alice');
INSERT INTO t1 VALUES (2, 'Bob');
INSERT INTO t1 VALUES (3, 'Charlie');
INSERT INTO t1 VALUES (4, 'David');
INSERT INTO t1 VALUES (5, 'Eve');
-- 2.2 NULL values.
INSERT INTO t1 VALUES (6, NULL);
INSERT INTO t1 VALUES (7, 'Frank');
INSERT INTO nullable_test VALUES (1, 10, 'hello');
INSERT INTO nullable_test VALUES (2, NULL, NULL);
INSERT INTO nullable_test VALUES (3, 30, NULL);
-- 2.3 Numeric types.
INSERT INTO t2 VALUES (1, 100, 1.5);
INSERT INTO t2 VALUES (2, 200, 2.5);
INSERT INTO t2 VALUES (3, 300, 3.5);
INSERT INTO t2 VALUES (4, 400, 4.5);
INSERT INTO t2 VALUES (5, 500, 5.5);
-- 2.4 Booleans and large integers.
INSERT INTO t3 VALUES (true, 1000000000, 3.14159);
INSERT INTO t3 VALUES (false, 2000000000, 2.71828);
INSERT INTO t3 VALUES (true, 3000000000, 1.41421);
-- 2.5 Bulk insert.
INSERT INTO all_types VALUES (true, 1, 100, 1.1, 1.11, 'row1');
INSERT INTO all_types VALUES (false, 2, 200, 2.2, 2.22, 'row2');
INSERT INTO all_types VALUES (true, 3, 300, 3.3, 3.33, 'row3');
INSERT INTO all_types VALUES (false, 4, 400, 4.4, 4.44, 'row4');
INSERT INTO all_types VALUES (true, 5, 500, 5.5, 5.55, 'row5');
-- 2.6 Multi-row VALUES.
INSERT INTO t4 VALUES (1, 'apple');
INSERT INTO t4 VALUES (2, 'banana');
INSERT INTO t4 VALUES (3, 'cherry');
INSERT INTO t4 VALUES (4, 'date');
INSERT INTO t4 VALUES (5, 'elderberry');
-- 2.7 JOIN fixture tables.
CREATE TABLE students (id INT PRIMARY KEY, name VARCHAR, age INT);
CREATE TABLE scores (id INT PRIMARY KEY, student_id INT, subject VARCHAR, score FLOAT);
INSERT INTO students VALUES (1, 'Alice', 20);
INSERT INTO students VALUES (2, 'Bob', 21);
INSERT INTO students VALUES (3, 'Charlie', 22);
INSERT INTO students VALUES (4, 'Diana', 20);
INSERT INTO students VALUES (5, 'Eve', 21);
INSERT INTO scores VALUES (1, 1, 'Math', 95.5);
INSERT INTO scores VALUES (2, 1, 'English', 88.0);
INSERT INTO scores VALUES (3, 2, 'Math', 78.5);
INSERT INTO scores VALUES (4, 2, 'English', 92.0);
INSERT INTO scores VALUES (5, 3, 'Math', 85.0);
INSERT INTO scores VALUES (6, 3, 'English', 90.5);
INSERT INTO scores VALUES (7, 4, 'Math', 92.0);
INSERT INTO scores VALUES (8, 5, 'English', 87.5);
-- 2.8 Aggregation fixture table.
CREATE TABLE employees (id INT PRIMARY KEY, name VARCHAR, dept VARCHAR, salary FLOAT, age INT);
INSERT INTO employees VALUES (1, 'Alice', 'Engineering', 90000, 30);
INSERT INTO employees VALUES (2, 'Bob', 'Engineering', 85000, 28);
INSERT INTO employees VALUES (3, 'Charlie', 'Sales', 70000, 35);
INSERT INTO employees VALUES (4, 'Diana', 'Sales', 75000, 32);
INSERT INTO employees VALUES (5, 'Eve', 'Marketing', 80000, 29);
INSERT INTO employees VALUES (6, 'Frank', 'Marketing', 82000, 31);
INSERT INTO employees VALUES (7, 'Grace', 'Engineering', 95000, 33);
INSERT INTO employees VALUES (8, 'Hank', 'Sales', 72000, 27);
INSERT INTO employees VALUES (9, 'Ivy', 'Engineering', 88000, 26);
INSERT INTO employees VALUES (10, 'Jack', 'Marketing', 78000, 34);
-- 2.9 Transaction fixture table.
CREATE TABLE accounts (id INT PRIMARY KEY, name VARCHAR, balance FLOAT);
INSERT INTO accounts VALUES (1, 'Alice', 1000.0);
INSERT INTO accounts VALUES (2, 'Bob', 2000.0);
INSERT INTO accounts VALUES (3, 'Charlie', 3000.0);
-- 3.1 SELECT *
SELECT * FROM t1;
SELECT * FROM t2;
SELECT * FROM t3;
SELECT * FROM students;
SELECT * FROM employees;
-- 3.2 Explicit column list.
SELECT id, name FROM t1;
SELECT name FROM t1;
SELECT a, b FROM t2;
SELECT name, dept FROM employees;
SELECT student_id, score FROM scores;
-- 3.3 Expression projection.
SELECT id, id + 1 FROM t1;
SELECT a, a * 2 FROM t2;
SELECT salary, salary * 1.1 FROM employees;
SELECT age, age + 10 AS future_age FROM students;
SELECT score, score - 10 FROM scores;
SELECT a, a / 2.0 FROM t2;
SELECT id, id * id FROM t1;
-- 3.4 Aliases.
SELECT id AS user_id, name AS user_name FROM t1;
SELECT a AS x, b AS y FROM t2;
SELECT name employee_name, dept department FROM employees;
-- 3.5 DISTINCT
SELECT DISTINCT age FROM students;
SELECT DISTINCT dept FROM employees;
SELECT DISTINCT subject FROM scores;
SELECT DISTINCT age FROM employees;
SELECT DISTINCT student_id FROM scores;
-- 4.1 Equality comparison.
SELECT * FROM t1 WHERE id = 1;
SELECT * FROM t1 WHERE name = 'Alice';
SELECT * FROM t2 WHERE a = 3;
SELECT * FROM employees WHERE dept = 'Engineering';
SELECT * FROM students WHERE age = 20;
-- 4.2 Inequality comparison.
SELECT * FROM t1 WHERE id != 1;
SELECT * FROM t1 WHERE id <> 1;
SELECT * FROM t2 WHERE a > 3;
SELECT * FROM t2 WHERE a < 3;
SELECT * FROM t2 WHERE a >= 3;
SELECT * FROM t2 WHERE a <= 3;
SELECT * FROM employees WHERE salary > 80000;
SELECT * FROM employees WHERE age < 30;
SELECT * FROM students WHERE age >= 21;
-- 4.3 AND
SELECT * FROM t1 WHERE id > 1 AND id < 5;
SELECT * FROM employees WHERE dept = 'Engineering' AND salary > 85000;
SELECT * FROM students WHERE age = 20 AND name = 'Alice';
SELECT * FROM t2 WHERE a > 1 AND b > 200;
SELECT * FROM scores WHERE subject = 'Math' AND score > 80;
SELECT * FROM employees WHERE dept = 'Sales' AND age > 30;
-- 4.4 OR
SELECT * FROM t1 WHERE id = 1 OR id = 3;
SELECT * FROM employees WHERE dept = 'Engineering' OR dept = 'Sales';
SELECT * FROM students WHERE age = 20 OR age = 22;
SELECT * FROM t2 WHERE a < 2 OR a > 4;
SELECT * FROM scores WHERE subject = 'Math' OR score > 90;
-- 4.5 NOT
SELECT * FROM t1 WHERE NOT (id = 1);
SELECT * FROM employees WHERE NOT (dept = 'Engineering');
SELECT * FROM students WHERE NOT (age = 20);
-- 4.6 BETWEEN
SELECT * FROM t1 WHERE id BETWEEN 2 AND 4;
SELECT * FROM employees WHERE salary BETWEEN 75000 AND 85000;
SELECT * FROM students WHERE age BETWEEN 20 AND 21;
SELECT * FROM t2 WHERE a BETWEEN 1 AND 3;
SELECT * FROM scores WHERE score BETWEEN 85 AND 95;
SELECT * FROM employees WHERE age BETWEEN 28 AND 32;
-- 4.7 IN (value list).
SELECT * FROM t1 WHERE id IN (1, 3, 5);
SELECT * FROM employees WHERE dept IN ('Engineering', 'Sales');
SELECT * FROM students WHERE age IN (20, 22);
SELECT * FROM t2 WHERE a IN (1, 2, 3);
SELECT * FROM scores WHERE subject IN ('Math', 'English');
SELECT * FROM employees WHERE id IN (1, 5, 10);
-- 4.8 LIKE
SELECT * FROM t1 WHERE name LIKE 'A%';
SELECT * FROM t1 WHERE name LIKE '%b';
SELECT * FROM t1 WHERE name LIKE '%li%';
SELECT * FROM employees WHERE name LIKE 'A%';
SELECT * FROM employees WHERE dept LIKE '%ing%';
SELECT * FROM t4 WHERE val LIKE 'a%';
SELECT * FROM t4 WHERE val LIKE '%rry';
-- 4.9 IS NULL / IS NOT NULL
SELECT * FROM t1 WHERE name IS NULL;
SELECT * FROM t1 WHERE name IS NOT NULL;
SELECT * FROM nullable_test WHERE a IS NULL;
SELECT * FROM nullable_test WHERE b IS NOT NULL;
SELECT * FROM nullable_test WHERE a IS NULL AND b IS NULL;
-- 4.10 Compound predicates.
SELECT * FROM employees WHERE (dept = 'Engineering' OR dept = 'Sales') AND salary > 80000;
SELECT * FROM students WHERE age IN (20, 21) AND name LIKE 'A%';
SELECT * FROM t2 WHERE a BETWEEN 2 AND 4 AND b > 200;
SELECT * FROM scores WHERE score > 85 AND subject = 'Math' OR subject = 'English';
SELECT * FROM employees WHERE salary > 80000 AND age < 33 AND dept != 'Sales';
-- 5.1 Basic UPDATE.
UPDATE t1 SET name = 'Alice2' WHERE id = 1;
SELECT * FROM t1 WHERE id = 1;
-- 5.2 Numeric UPDATE.
UPDATE t2 SET b = 999 WHERE a = 1;
SELECT * FROM t2 WHERE a = 1;
-- 5.3 Expression UPDATE.
UPDATE t2 SET b = b + 100 WHERE a = 2;
SELECT * FROM t2 WHERE a = 2;
UPDATE employees SET salary = salary * 1.1 WHERE dept = 'Engineering';
SELECT * FROM employees WHERE dept = 'Engineering';
-- 5.4 Multi-column UPDATE.
UPDATE employees SET salary = 95000, age = 31 WHERE id = 1;
SELECT * FROM employees WHERE id = 1;
-- 5.5 NULL UPDATE.
UPDATE t1 SET name = NULL WHERE id = 6;
SELECT * FROM t1 WHERE id = 6;
-- 5.6 Conditional UPDATE.
UPDATE employees SET salary = salary + 5000 WHERE age > 30;
SELECT * FROM employees WHERE age > 30;
UPDATE students SET age = age + 1 WHERE name = 'Alice';
SELECT * FROM students WHERE name = 'Alice';
-- 5.7 HOT UPDATE (non-indexed column).
UPDATE employees SET name = 'Alice_v2' WHERE id = 1;
SELECT * FROM employees WHERE id = 1;
-- 5.8 Bulk UPDATE.
UPDATE t2 SET c = c * 2;
SELECT * FROM t2;
-- 6.1 Basic DELETE.
CREATE TABLE del_test (id INT, val VARCHAR);
INSERT INTO del_test VALUES (1, 'a');
INSERT INTO del_test VALUES (2, 'b');
INSERT INTO del_test VALUES (3, 'c');
INSERT INTO del_test VALUES (4, 'd');
INSERT INTO del_test VALUES (5, 'e');
DELETE FROM del_test WHERE id = 3;
SELECT * FROM del_test;
-- 6.2 Conditional DELETE.
DELETE FROM del_test WHERE id > 3;
SELECT * FROM del_test;
-- 6.3 INSERT after DELETE.
INSERT INTO del_test VALUES (10, 'new');
SELECT * FROM del_test;
-- 6.4 DELETE all rows.
DELETE FROM del_test;
SELECT * FROM del_test;
-- 7.1 INNER JOIN
SELECT s.name, sc.subject, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id;
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id WHERE sc.score > 90;
SELECT s.name, sc.subject FROM students s INNER JOIN scores sc ON s.id = sc.student_id WHERE s.age = 20;
-- 7.2 LEFT JOIN
SELECT s.name, sc.subject, sc.score FROM students s LEFT JOIN scores sc ON s.id = sc.student_id;
SELECT s.name, sc.score FROM students s LEFT JOIN scores sc ON s.id = sc.student_id WHERE sc.score IS NULL;
-- 7.3 JOIN + WHERE
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id WHERE sc.subject = 'Math';
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id WHERE s.age > 20 AND sc.score > 80;
-- 7.4 JOIN + ORDER BY
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id ORDER BY sc.score DESC;
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id ORDER BY s.name ASC;
-- 7.5 JOIN + LIMIT
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id ORDER BY sc.score DESC LIMIT 3;
-- 7.6 JOIN + aggregation.
SELECT s.name, COUNT(*), AVG(sc.score) FROM students s INNER JOIN scores sc ON s.id = sc.student_id GROUP BY s.name;
SELECT s.name, MAX(sc.score) FROM students s INNER JOIN scores sc ON s.id = sc.student_id GROUP BY s.name HAVING MAX(sc.score) > 90;
-- 7.7 Three-way JOIN (chained twice).
CREATE TABLE classes (id INT, student_id INT, room VARCHAR);
INSERT INTO classes VALUES (1, 1, 'A101');
INSERT INTO classes VALUES (2, 2, 'A102');
INSERT INTO classes VALUES (3, 3, 'A101');
SELECT s.name, c.room, sc.score FROM students s INNER JOIN classes c ON s.id = c.student_id INNER JOIN scores sc ON s.id = sc.student_id;
-- 7.8 Self-join.
SELECT a.name AS student1, b.name AS student2 FROM students a INNER JOIN students b ON a.age = b.age WHERE a.id < b.id;
-- 8.1 COUNT
SELECT COUNT(*) FROM t1;
SELECT COUNT(*) FROM employees;
SELECT COUNT(*) FROM scores;
SELECT COUNT(*) FROM students;
-- 8.2 SUM
SELECT SUM(b) FROM t2;
SELECT SUM(salary) FROM employees;
SELECT SUM(score) FROM scores;
-- 8.3 AVG
SELECT AVG(salary) FROM employees;
SELECT AVG(score) FROM scores;
SELECT AVG(age) FROM students;
SELECT AVG(b) FROM t2;
-- 8.4 MIN / MAX
SELECT MIN(salary) FROM employees;
SELECT MAX(salary) FROM employees;
SELECT MIN(age) FROM students;
SELECT MAX(age) FROM students;
SELECT MIN(score) FROM scores;
SELECT MAX(score) FROM scores;
-- 8.5 Multiple aggregates.
SELECT COUNT(*), SUM(salary), AVG(salary), MIN(salary), MAX(salary) FROM employees;
SELECT COUNT(*), AVG(score), MIN(score), MAX(score) FROM scores;
-- 8.6 GROUP BY
SELECT dept, COUNT(*) FROM employees GROUP BY dept;
SELECT dept, AVG(salary) FROM employees GROUP BY dept;
SELECT dept, SUM(salary) FROM employees GROUP BY dept;
SELECT age, COUNT(*) FROM students GROUP BY age;
SELECT subject, AVG(score) FROM scores GROUP BY subject;
SELECT student_id, COUNT(*) FROM scores GROUP BY student_id;
SELECT student_id, AVG(score) FROM scores GROUP BY student_id;
-- 8.7 HAVING
SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 2;
SELECT dept, AVG(salary) FROM employees GROUP BY dept HAVING AVG(salary) > 80000;
SELECT student_id, AVG(score) FROM scores GROUP BY student_id HAVING AVG(score) > 85;
SELECT subject, COUNT(*) FROM scores GROUP BY subject HAVING COUNT(*) >= 3;
-- 8.8 GROUP BY + ORDER BY
SELECT dept, AVG(salary) AS avg_sal FROM employees GROUP BY dept ORDER BY avg_sal DESC;
SELECT student_id, AVG(score) AS avg_score FROM scores GROUP BY student_id ORDER BY avg_score DESC;
-- 9.1 ORDER BY ASC
SELECT * FROM t1 ORDER BY id ASC;
SELECT * FROM employees ORDER BY name ASC;
SELECT * FROM students ORDER BY age ASC;
-- 9.2 ORDER BY DESC
SELECT * FROM t1 ORDER BY id DESC;
SELECT * FROM employees ORDER BY salary DESC;
SELECT * FROM scores ORDER BY score DESC;
-- 9.3 Multi-column ORDER BY.
SELECT * FROM employees ORDER BY dept ASC, salary DESC;
SELECT * FROM students ORDER BY age ASC, name ASC;
SELECT * FROM scores ORDER BY subject ASC, score DESC;
-- 9.4 LIMIT
SELECT * FROM t1 ORDER BY id LIMIT 3;
SELECT * FROM employees ORDER BY salary DESC LIMIT 5;
SELECT * FROM scores ORDER BY score DESC LIMIT 3;
-- 9.5 OFFSET
SELECT * FROM t1 ORDER BY id LIMIT 3 OFFSET 2;
SELECT * FROM employees ORDER BY salary DESC LIMIT 3 OFFSET 2;
SELECT * FROM scores ORDER BY score DESC LIMIT 5 OFFSET 3;
-- 9.6 LIMIT + WHERE
SELECT * FROM employees WHERE dept = 'Engineering' ORDER BY salary DESC LIMIT 2;
SELECT * FROM scores WHERE subject = 'Math' ORDER BY score DESC LIMIT 3;
-- 10.1 UNION ALL
SELECT name FROM students WHERE age = 20 UNION ALL SELECT name FROM students WHERE age = 21;
SELECT name FROM employees WHERE dept = 'Engineering' UNION ALL SELECT name FROM employees WHERE dept = 'Sales';
-- 10.2 UNION (distinct).
SELECT age FROM students WHERE age = 20 UNION SELECT age FROM students WHERE age = 21;
SELECT age FROM students WHERE age = 20 UNION SELECT age FROM students WHERE age = 20;
-- 10.3 UNION + ORDER BY
SELECT name FROM students WHERE age = 20 UNION ALL SELECT name FROM students WHERE age = 21;
-- 10.4 Cross-table UNION.
SELECT name FROM students UNION ALL SELECT name FROM employees;
-- 11.1 CASE WHEN
SELECT name, CASE WHEN age >= 21 THEN 'adult' ELSE 'minor' END AS category FROM students;
SELECT name, CASE WHEN salary > 85000 THEN 'high' WHEN salary > 75000 THEN 'medium' ELSE 'low' END AS level FROM employees;
SELECT name, CASE WHEN age < 28 THEN 'young' WHEN age < 32 THEN 'middle' ELSE 'senior' END AS age_group FROM employees;
-- 11.2 CASE WHEN + WHERE
SELECT name, CASE WHEN salary > 80000 THEN 'A' ELSE 'B' END AS grade FROM employees WHERE dept = 'Engineering';
-- 11.3 COALESCE
SELECT COALESCE(name, 'unknown') FROM t1;
SELECT COALESCE(a, 0) FROM nullable_test;
SELECT COALESCE(b, 'empty') FROM nullable_test;
-- 11.4 NULLIF
SELECT NULLIF(age, 20) FROM students;
SELECT NULLIF(dept, 'Sales') FROM employees;
-- 12.1 IN (SELECT ...)
SELECT * FROM students WHERE id IN (SELECT student_id FROM scores WHERE score > 90);
SELECT * FROM employees WHERE id IN (SELECT id FROM employees WHERE dept = 'Engineering');
SELECT * FROM students WHERE id IN (SELECT student_id FROM scores WHERE subject = 'Math');
-- 12.2 IN subquery + WHERE.
SELECT * FROM students WHERE id IN (SELECT student_id FROM scores WHERE score > 85) AND age > 20;
-- 13.1 COMMIT
BEGIN;
INSERT INTO accounts VALUES (4, 'Dave', 4000.0);
SELECT * FROM accounts WHERE id = 4;
COMMIT;
SELECT * FROM accounts WHERE id = 4;
-- 13.2 ROLLBACK
BEGIN;
INSERT INTO accounts VALUES (5, 'Eve', 5000.0);
SELECT * FROM accounts WHERE id = 5;
ROLLBACK;
SELECT * FROM accounts WHERE id = 5;
-- 13.3 UPDATE inside a transaction.
BEGIN;
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
UPDATE accounts SET balance = balance + 100 WHERE id = 2;
SELECT * FROM accounts;
COMMIT;
SELECT * FROM accounts;
-- 13.4 ROLLBACK of an in-transaction UPDATE.
BEGIN;
UPDATE accounts SET balance = 0 WHERE id = 1;
SELECT * FROM accounts WHERE id = 1;
ROLLBACK;
SELECT * FROM accounts WHERE id = 1;
-- 13.5 DELETE inside a transaction.
BEGIN;
DELETE FROM accounts WHERE id = 3;
SELECT * FROM accounts;
ROLLBACK;
SELECT * FROM accounts WHERE id = 3;
-- 14.1 EXPLAIN SELECT
EXPLAIN SELECT * FROM t1;
EXPLAIN SELECT * FROM t1 WHERE id = 1;
EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering';
EXPLAIN SELECT * FROM scores WHERE score > 90;
-- 14.2 EXPLAIN JOIN
EXPLAIN SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id;
-- 14.3 EXPLAIN on aggregation.
EXPLAIN SELECT COUNT(*), AVG(salary) FROM employees;
EXPLAIN SELECT dept, COUNT(*) FROM employees GROUP BY dept;
-- 14.4 EXPLAIN on a subquery.
EXPLAIN SELECT * FROM students WHERE id IN (SELECT student_id FROM scores WHERE score > 90);
-- 15.1 Query an empty table.
CREATE TABLE empty_table (id INT, val VARCHAR);
SELECT * FROM empty_table;
SELECT COUNT(*) FROM empty_table;
SELECT * FROM empty_table WHERE id = 1;
-- 15.2 NULL handling.
SELECT * FROM t1 WHERE name IS NULL;
SELECT * FROM t1 WHERE name IS NOT NULL;
SELECT * FROM nullable_test WHERE a IS NULL;
SELECT * FROM nullable_test WHERE a IS NOT NULL;
SELECT COALESCE(NULL, 1);
SELECT NULLIF(1, 1);
SELECT NULLIF(1, 2);
-- 15.3 Large numeric values.
INSERT INTO t2 VALUES (100, 999999, 999999.99);
INSERT INTO t2 VALUES (101, -999999, -999999.99);
SELECT * FROM t2 WHERE a >= 100;
-- 15.4 Empty string.
INSERT INTO t1 VALUES (100, '');
SELECT * FROM t1 WHERE name = '';
SELECT * FROM t1 WHERE name LIKE '%';
-- 15.5 Duplicate insert.
INSERT INTO t1 VALUES (200, 'dup');
INSERT INTO t1 VALUES (200, 'dup2');
SELECT * FROM t1 WHERE id = 200;
-- 15.6 INSERT after DELETE.
CREATE TABLE reuse_test (id INT PRIMARY KEY, val VARCHAR);
INSERT INTO reuse_test VALUES (1, 'first');
DELETE FROM reuse_test WHERE id = 1;
INSERT INTO reuse_test VALUES (1, 'second');
SELECT * FROM reuse_test;
-- 15.7 Repeated UPDATEs.
CREATE TABLE multi_update (id INT, counter INT);
INSERT INTO multi_update VALUES (1, 0);
UPDATE multi_update SET counter = counter + 1 WHERE id = 1;
UPDATE multi_update SET counter = counter + 1 WHERE id = 1;
UPDATE multi_update SET counter = counter + 1 WHERE id = 1;
SELECT * FROM multi_update;
-- 15.8 GROUP BY with single-row result.
SELECT dept, COUNT(*) FROM employees WHERE dept = 'Engineering' GROUP BY dept;
-- 15.9 HAVING filters every group.
SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 100;
-- 15.10 LIMIT 0
SELECT * FROM t1 LIMIT 0;
-- 15.11 OFFSET beyond row count.
SELECT * FROM t1 LIMIT 5 OFFSET 1000;
-- 16.1 WHERE + ORDER BY + LIMIT
SELECT * FROM employees WHERE salary > 75000 ORDER BY salary DESC LIMIT 3;
SELECT * FROM scores WHERE subject = 'Math' ORDER BY score DESC LIMIT 2;
SELECT * FROM students WHERE age >= 20 ORDER BY name ASC LIMIT 3;
-- 16.2 WHERE + GROUP BY + HAVING
SELECT dept, AVG(salary) FROM employees WHERE age > 28 GROUP BY dept HAVING AVG(salary) > 80000;
SELECT subject, COUNT(*) FROM scores WHERE score > 80 GROUP BY subject HAVING COUNT(*) >= 2;
-- 16.3 JOIN + WHERE + ORDER BY
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id WHERE sc.score > 85 ORDER BY sc.score DESC;
-- 16.4 JOIN + GROUP BY
SELECT s.name, COUNT(*) AS cnt, AVG(sc.score) AS avg_s FROM students s INNER JOIN scores sc ON s.id = sc.student_id GROUP BY s.name ORDER BY avg_s DESC;
-- 16.5 Subquery + JOIN.
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id WHERE s.id IN (SELECT student_id FROM scores WHERE score > 90);
-- 16.6 Nested subqueries.
SELECT * FROM employees WHERE dept IN ('Engineering', 'Sales') AND salary > (SELECT AVG(salary) FROM employees);
-- 16.7 Expression + aggregation.
SELECT dept, SUM(salary * 1.1) FROM employees GROUP BY dept;
SELECT dept, AVG(salary) - MIN(salary) AS salary_range FROM employees GROUP BY dept;
-- 16.8 CASE + aggregation.
SELECT dept, SUM(CASE WHEN salary > 85000 THEN 1 ELSE 0 END) AS high_earners FROM employees GROUP BY dept;
-- 16.9 DISTINCT + ORDER BY
SELECT DISTINCT dept FROM employees ORDER BY dept ASC;
SELECT DISTINCT age FROM students ORDER BY age DESC;
-- 16.10 UNION + ORDER BY
SELECT name FROM students WHERE age = 20 UNION ALL SELECT name FROM students WHERE age = 21;
-- 17.1 CREATE -> INSERT -> SELECT -> UPDATE -> DELETE -> SELECT.
CREATE TABLE lifecycle (id INT PRIMARY KEY, val VARCHAR);
INSERT INTO lifecycle VALUES (1, 'created');
SELECT * FROM lifecycle;
UPDATE lifecycle SET val = 'updated' WHERE id = 1;
SELECT * FROM lifecycle;
DELETE FROM lifecycle WHERE id = 1;
SELECT * FROM lifecycle;
DROP TABLE lifecycle;
-- 17.2 Index + SELECT.
CREATE TABLE idx_test (id INT PRIMARY KEY, name VARCHAR, score INT);
CREATE INDEX idx_name ON idx_test (name);
INSERT INTO idx_test VALUES (1, 'Alice', 90);
INSERT INTO idx_test VALUES (2, 'Bob', 85);
INSERT INTO idx_test VALUES (3, 'Charlie', 95);
SELECT * FROM idx_test WHERE name = 'Alice';
EXPLAIN SELECT * FROM idx_test WHERE name = 'Alice';
-- 17.3 Transaction + index.
BEGIN;
INSERT INTO idx_test VALUES (4, 'Dave', 80);
SELECT * FROM idx_test WHERE name = 'Dave';
ROLLBACK;
SELECT * FROM idx_test WHERE name = 'Dave';
-- 18.1 BOOL
CREATE TABLE bool_test (id INT, flag BOOL);
INSERT INTO bool_test VALUES (1, true);
INSERT INTO bool_test VALUES (2, false);
SELECT * FROM bool_test WHERE flag = true;
SELECT * FROM bool_test WHERE flag = false;
-- 18.2 BIGINT
CREATE TABLE bigint_test (id INT, big BIGINT);
INSERT INTO bigint_test VALUES (1, 9223372036854775807);
INSERT INTO bigint_test VALUES (2, -9223372036854775808);
SELECT * FROM bigint_test;
-- 18.3 FLOAT / DOUBLE
CREATE TABLE float_test (id INT, f FLOAT, d DOUBLE);
INSERT INTO float_test VALUES (1, 3.14, 3.14159265358979);
INSERT INTO float_test VALUES (2, 2.72, 2.71828182845905);
INSERT INTO float_test VALUES (3, 1.41, 1.41421356237310);
SELECT * FROM float_test WHERE f > 2.0;
SELECT * FROM float_test WHERE d < 3.0;
SELECT AVG(f), AVG(d) FROM float_test;
-- 18.4 VARCHAR
CREATE TABLE varchar_test (id INT, s VARCHAR);
INSERT INTO varchar_test VALUES (1, 'hello');
INSERT INTO varchar_test VALUES (2, 'world');
INSERT INTO varchar_test VALUES (3, 'hello world');
INSERT INTO varchar_test VALUES (4, '');
INSERT INTO varchar_test VALUES (5, NULL);
SELECT * FROM varchar_test WHERE s LIKE '%world%';
SELECT * FROM varchar_test WHERE s IS NULL;
SELECT * FROM varchar_test WHERE s = '';
-- 18.5 Mixed-type comparisons.
SELECT * FROM float_test WHERE f > 2 AND d < 4.0;
-- 19.1 Bulk INSERT.
CREATE TABLE perf_test (id INT, val INT);
INSERT INTO perf_test VALUES (1, 100);
INSERT INTO perf_test VALUES (2, 200);
INSERT INTO perf_test VALUES (3, 300);
INSERT INTO perf_test VALUES (4, 400);
INSERT INTO perf_test VALUES (5, 500);
INSERT INTO perf_test VALUES (6, 600);
INSERT INTO perf_test VALUES (7, 700);
INSERT INTO perf_test VALUES (8, 800);
INSERT INTO perf_test VALUES (9, 900);
INSERT INTO perf_test VALUES (10, 1000);
-- 19.2 Full table scan.
SELECT * FROM perf_test;
SELECT COUNT(*) FROM perf_test;
SELECT AVG(val) FROM perf_test;
-- 19.3 Range query.
SELECT * FROM perf_test WHERE val BETWEEN 300 AND 700;
SELECT * FROM perf_test WHERE id > 5;
SELECT * FROM perf_test WHERE val < 500;
-- 19.4 ORDER BY.
SELECT * FROM perf_test ORDER BY val DESC;
SELECT * FROM perf_test ORDER BY id ASC LIMIT 5;
-- 19.5 Repeated UPDATEs.
UPDATE perf_test SET val = val + 1 WHERE id = 1;
UPDATE perf_test SET val = val + 1 WHERE id = 1;
UPDATE perf_test SET val = val + 1 WHERE id = 1;
SELECT * FROM perf_test WHERE id = 1;
-- 19.6 DELETE + INSERT loop.
DELETE FROM perf_test WHERE id = 10;
INSERT INTO perf_test VALUES (10, 1000);
SELECT * FROM perf_test WHERE id = 10;
-- 20.1 Non-existent table.
SELECT * FROM nonexistent;
INSERT INTO nonexistent VALUES (1);
-- 20.2 Syntax error.
SELECT FROM t1;
INSERT t1 VALUES (1);
CREATE t1;
-- 20.3 Duplicate primary key.
CREATE TABLE dup_pk (id INT PRIMARY KEY, val VARCHAR);
INSERT INTO dup_pk VALUES (1, 'first');
INSERT INTO dup_pk VALUES (1, 'second');
-- 20.4 NOT NULL violation.
INSERT INTO t4 VALUES (100, NULL);
-- 20.5 Type mismatch (should be handled).
INSERT INTO t1 VALUES ('not_an_int', 'test');
-- 20.6 Unsupported command.
ALTER TABLE t1 ADD COLUMN new_col INT;
SHOW DATABASES;
USE test;
-- 21.1 E-commerce scenario.
CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR, price FLOAT, category VARCHAR, stock INT);
CREATE TABLE order_items (id INT PRIMARY KEY, order_id INT, product_id INT, quantity INT);
INSERT INTO products VALUES (1, 'Laptop', 999.99, 'Electronics', 50);
INSERT INTO products VALUES (2, 'Phone', 699.99, 'Electronics', 100);
INSERT INTO products VALUES (3, 'Book', 19.99, 'Education', 200);
INSERT INTO products VALUES (4, 'Pen', 2.99, 'Education', 500);
INSERT INTO products VALUES (5, 'Desk', 299.99, 'Furniture', 30);
INSERT INTO order_items VALUES (1, 101, 1, 2);
INSERT INTO order_items VALUES (2, 101, 3, 5);
INSERT INTO order_items VALUES (3, 102, 2, 1);
INSERT INTO order_items VALUES (4, 102, 4, 10);
INSERT INTO order_items VALUES (5, 103, 1, 1);
INSERT INTO order_items VALUES (6, 103, 5, 1);
-- Query: items per order.
SELECT p.name, oi.quantity, p.price, oi.quantity * p.price AS total FROM products p INNER JOIN order_items oi ON p.id = oi.product_id;
-- Query: total amount per order.
SELECT oi.order_id, SUM(oi.quantity * p.price) AS order_total FROM products p INNER JOIN order_items oi ON p.id = oi.product_id GROUP BY oi.order_id;
-- Query: sales count per category.
SELECT p.category, SUM(oi.quantity) AS total_sold FROM products p INNER JOIN order_items oi ON p.id = oi.product_id GROUP BY p.category;
-- Query: top-3 best sellers.
SELECT p.name, SUM(oi.quantity) AS sold FROM products p INNER JOIN order_items oi ON p.id = oi.product_id GROUP BY p.name ORDER BY sold DESC LIMIT 3;
-- Query: low-stock items.
SELECT name, stock FROM products WHERE stock < 50;
-- Update: promotional 10% discount.
UPDATE products SET price = price * 0.9 WHERE category = 'Electronics';
SELECT * FROM products WHERE category = 'Electronics';
-- 21.2 Student grades scenario.
CREATE TABLE courses (id INT PRIMARY KEY, name VARCHAR, credits INT);
INSERT INTO courses VALUES (1, 'Math', 4);
INSERT INTO courses VALUES (2, 'English', 3);
INSERT INTO courses VALUES (3, 'Physics', 4);
CREATE TABLE enrollments (student_id INT, course_id INT, grade FLOAT);
INSERT INTO enrollments VALUES (1, 1, 95.0);
INSERT INTO enrollments VALUES (1, 2, 88.0);
INSERT INTO enrollments VALUES (2, 1, 78.0);
INSERT INTO enrollments VALUES (2, 3, 82.0);
INSERT INTO enrollments VALUES (3, 1, 92.0);
INSERT INTO enrollments VALUES (3, 2, 90.0);
INSERT INTO enrollments VALUES (3, 3, 85.0);
-- GPA computation.
SELECT s.name, AVG(e.grade) AS gpa FROM students s INNER JOIN enrollments e ON s.id = e.student_id GROUP BY s.name ORDER BY gpa DESC;
-- Average score per course.
SELECT c.name, AVG(e.grade) FROM courses c INNER JOIN enrollments e ON c.id = e.course_id GROUP BY c.name;
-- Failing students (grade < 80).
SELECT s.name, c.name, e.grade FROM students s INNER JOIN enrollments e ON s.id = e.student_id INNER JOIN courses c ON e.course_id = c.id WHERE e.grade < 80;
-- 21.3 Inventory management.
CREATE TABLE inventory (id INT PRIMARY KEY, product VARCHAR, quantity INT, warehouse VARCHAR);
INSERT INTO inventory VALUES (1, 'Widget', 100, 'A');
INSERT INTO inventory VALUES (2, 'Gadget', 50, 'A');
INSERT INTO inventory VALUES (3, 'Widget', 200, 'B');
INSERT INTO inventory VALUES (4, 'Gadget', 75, 'B');
INSERT INTO inventory VALUES (5, 'Thingamajig', 30, 'A');
-- Total stock per warehouse.
SELECT warehouse, SUM(quantity) FROM inventory GROUP BY warehouse;
-- Total stock per product.
SELECT product, SUM(quantity) FROM inventory GROUP BY product ORDER BY SUM(quantity) DESC;
-- Products with stock below 100.
SELECT product, warehouse, quantity FROM inventory WHERE quantity < 100 ORDER BY quantity ASC;
-- Stock transfer.
BEGIN;
UPDATE inventory SET quantity = quantity - 20 WHERE product = 'Widget' AND warehouse = 'A';
UPDATE inventory SET quantity = quantity + 20 WHERE product = 'Widget' AND warehouse = 'B';
COMMIT;
SELECT * FROM inventory WHERE product = 'Widget';
-- Confirm previously landed fixes still hold.
-- 22.1 FLOAT serialisation.
CREATE TABLE float_reg (id INT, val FLOAT);
INSERT INTO float_reg VALUES (1, 123.456);
INSERT INTO float_reg VALUES (2, 0.001);
INSERT INTO float_reg VALUES (3, -999.99);
SELECT * FROM float_reg;
-- 22.2 COUNT(*) does not crash.
SELECT COUNT(*) FROM float_reg;
SELECT COUNT(*), AVG(val) FROM float_reg;
-- 22.3 Repeated UPDATE + SELECT.
CREATE TABLE update_reg (id INT, val INT);
INSERT INTO update_reg VALUES (1, 10);
UPDATE update_reg SET val = 20 WHERE id = 1;
UPDATE update_reg SET val = 30 WHERE id = 1;
UPDATE update_reg SET val = 40 WHERE id = 1;
SELECT * FROM update_reg WHERE id = 1;
-- 22.4 Transaction rollback.
BEGIN;
INSERT INTO update_reg VALUES (2, 999);
ROLLBACK;
SELECT * FROM update_reg WHERE id = 2;
-- 22.5 DELETE + INSERT
DELETE FROM update_reg WHERE id = 1;
INSERT INTO update_reg VALUES (1, 100);
SELECT * FROM update_reg WHERE id = 1;
-- 22.6 SHOW TABLES does not crash.
SHOW TABLES;
-- 22.7 DESC does not crash.
DESC update_reg;
DESC float_reg;
-- Final state check.
SHOW TABLES;
SELECT COUNT(*) FROM t1;
SELECT COUNT(*) FROM employees;
SELECT COUNT(*) FROM students;
SELECT COUNT(*) FROM scores;
SELECT COUNT(*) FROM products;
SELECT COUNT(*) FROM accounts;
-- Cleanup.
DROP TABLE empty_table;
DROP TABLE del_test;
DROP TABLE perf_test;
DROP TABLE bool_test;
DROP TABLE bigint_test;
DROP TABLE float_test;
DROP TABLE varchar_test;
DROP TABLE reuse_test;
DROP TABLE multi_update;
DROP TABLE float_reg;
DROP TABLE update_reg;
SHOW TABLES;
