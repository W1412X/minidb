-- 1.1 基础建表
CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR);
CREATE TABLE t2 (a INT, b INT, c FLOAT);
CREATE TABLE t3 (x BOOL, y BIGINT, z DOUBLE);
CREATE TABLE t4 (id INT PRIMARY KEY, val VARCHAR NOT NULL);
CREATE TABLE t5 (a INT PRIMARY KEY, b INT UNIQUE, c VARCHAR);
-- 1.2 多列类型
CREATE TABLE all_types (c1 BOOL, c2 INT, c3 BIGINT, c4 FLOAT, c5 DOUBLE, c6 VARCHAR);
CREATE TABLE nullable_test (id INT, a INT, b VARCHAR);
CREATE TABLE pk_test (id INT PRIMARY KEY, data VARCHAR);
CREATE TABLE unique_test (id INT, email VARCHAR UNIQUE);
CREATE TABLE multi_col (a INT, b INT, c INT, d VARCHAR, e FLOAT);
-- 1.3 查看表结构
SHOW TABLES;
DESC t1;
DESC t2;
DESC t3;
DESC t4;
DESC t5;
DESC all_types;
-- 1.4 创建索引
CREATE INDEX idx_t2_a ON t2 (a);
CREATE INDEX idx_t2_b ON t2 (b);
CREATE UNIQUE INDEX idx_t4_val ON t4 (val);
CREATE INDEX idx_multi_a ON multi_col (a);
-- 1.5 删除表
CREATE TABLE drop_test (id INT);
DROP TABLE drop_test;
SHOW TABLES;
-- 2.1 基础插入
INSERT INTO t1 VALUES (1, 'Alice');
INSERT INTO t1 VALUES (2, 'Bob');
INSERT INTO t1 VALUES (3, 'Charlie');
INSERT INTO t1 VALUES (4, 'David');
INSERT INTO t1 VALUES (5, 'Eve');
-- 2.2 NULL 值
INSERT INTO t1 VALUES (6, NULL);
INSERT INTO t1 VALUES (7, 'Frank');
INSERT INTO nullable_test VALUES (1, 10, 'hello');
INSERT INTO nullable_test VALUES (2, NULL, NULL);
INSERT INTO nullable_test VALUES (3, 30, NULL);
-- 2.3 数值类型
INSERT INTO t2 VALUES (1, 100, 1.5);
INSERT INTO t2 VALUES (2, 200, 2.5);
INSERT INTO t2 VALUES (3, 300, 3.5);
INSERT INTO t2 VALUES (4, 400, 4.5);
INSERT INTO t2 VALUES (5, 500, 5.5);
-- 2.4 布尔和大整数
INSERT INTO t3 VALUES (true, 1000000000, 3.14159);
INSERT INTO t3 VALUES (false, 2000000000, 2.71828);
INSERT INTO t3 VALUES (true, 3000000000, 1.41421);
-- 2.5 大批量插入
INSERT INTO all_types VALUES (true, 1, 100, 1.1, 1.11, 'row1');
INSERT INTO all_types VALUES (false, 2, 200, 2.2, 2.22, 'row2');
INSERT INTO all_types VALUES (true, 3, 300, 3.3, 3.33, 'row3');
INSERT INTO all_types VALUES (false, 4, 400, 4.4, 4.44, 'row4');
INSERT INTO all_types VALUES (true, 5, 500, 5.5, 5.55, 'row5');
-- 2.6 多行数据
INSERT INTO t4 VALUES (1, 'apple');
INSERT INTO t4 VALUES (2, 'banana');
INSERT INTO t4 VALUES (3, 'cherry');
INSERT INTO t4 VALUES (4, 'date');
INSERT INTO t4 VALUES (5, 'elderberry');
-- 2.7 JOIN 测试表
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
-- 2.8 聚合测试表
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
-- 2.9 事务测试表
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
-- 3.2 指定列
SELECT id, name FROM t1;
SELECT name FROM t1;
SELECT a, b FROM t2;
SELECT name, dept FROM employees;
SELECT student_id, score FROM scores;
-- 3.3 表达式投影
SELECT id, id + 1 FROM t1;
SELECT a, a * 2 FROM t2;
SELECT salary, salary * 1.1 FROM employees;
SELECT age, age + 10 AS future_age FROM students;
SELECT score, score - 10 FROM scores;
SELECT a, a / 2.0 FROM t2;
SELECT id, id * id FROM t1;
-- 3.4 别名
SELECT id AS user_id, name AS user_name FROM t1;
SELECT a AS x, b AS y FROM t2;
SELECT name employee_name, dept department FROM employees;
-- 3.5 DISTINCT
SELECT DISTINCT age FROM students;
SELECT DISTINCT dept FROM employees;
SELECT DISTINCT subject FROM scores;
SELECT DISTINCT age FROM employees;
SELECT DISTINCT student_id FROM scores;
-- 4.1 等值比较
SELECT * FROM t1 WHERE id = 1;
SELECT * FROM t1 WHERE name = 'Alice';
SELECT * FROM t2 WHERE a = 3;
SELECT * FROM employees WHERE dept = 'Engineering';
SELECT * FROM students WHERE age = 20;
-- 4.2 不等比较
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
-- 4.7 IN (值列表)
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
-- 4.10 复合条件
SELECT * FROM employees WHERE (dept = 'Engineering' OR dept = 'Sales') AND salary > 80000;
SELECT * FROM students WHERE age IN (20, 21) AND name LIKE 'A%';
SELECT * FROM t2 WHERE a BETWEEN 2 AND 4 AND b > 200;
SELECT * FROM scores WHERE score > 85 AND subject = 'Math' OR subject = 'English';
SELECT * FROM employees WHERE salary > 80000 AND age < 33 AND dept != 'Sales';
-- 5.1 基础更新
UPDATE t1 SET name = 'Alice2' WHERE id = 1;
SELECT * FROM t1 WHERE id = 1;
-- 5.2 数值更新
UPDATE t2 SET b = 999 WHERE a = 1;
SELECT * FROM t2 WHERE a = 1;
-- 5.3 表达式更新
UPDATE t2 SET b = b + 100 WHERE a = 2;
SELECT * FROM t2 WHERE a = 2;
UPDATE employees SET salary = salary * 1.1 WHERE dept = 'Engineering';
SELECT * FROM employees WHERE dept = 'Engineering';
-- 5.4 多列更新
UPDATE employees SET salary = 95000, age = 31 WHERE id = 1;
SELECT * FROM employees WHERE id = 1;
-- 5.5 NULL 更新
UPDATE t1 SET name = NULL WHERE id = 6;
SELECT * FROM t1 WHERE id = 6;
-- 5.6 条件更新
UPDATE employees SET salary = salary + 5000 WHERE age > 30;
SELECT * FROM employees WHERE age > 30;
UPDATE students SET age = age + 1 WHERE name = 'Alice';
SELECT * FROM students WHERE name = 'Alice';
-- 5.7 HOT 更新 (非索引列)
UPDATE employees SET name = 'Alice_v2' WHERE id = 1;
SELECT * FROM employees WHERE id = 1;
-- 5.8 批量更新
UPDATE t2 SET c = c * 2;
SELECT * FROM t2;
-- 6.1 基础删除
CREATE TABLE del_test (id INT, val VARCHAR);
INSERT INTO del_test VALUES (1, 'a');
INSERT INTO del_test VALUES (2, 'b');
INSERT INTO del_test VALUES (3, 'c');
INSERT INTO del_test VALUES (4, 'd');
INSERT INTO del_test VALUES (5, 'e');
DELETE FROM del_test WHERE id = 3;
SELECT * FROM del_test;
-- 6.2 条件删除
DELETE FROM del_test WHERE id > 3;
SELECT * FROM del_test;
-- 6.3 删除后插入
INSERT INTO del_test VALUES (10, 'new');
SELECT * FROM del_test;
-- 6.4 删除所有
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
-- 7.6 JOIN + 聚合
SELECT s.name, COUNT(*), AVG(sc.score) FROM students s INNER JOIN scores sc ON s.id = sc.student_id GROUP BY s.name;
SELECT s.name, MAX(sc.score) FROM students s INNER JOIN scores sc ON s.id = sc.student_id GROUP BY s.name HAVING MAX(sc.score) > 90;
-- 7.7 三表 JOIN (用两次 JOIN)
CREATE TABLE classes (id INT, student_id INT, room VARCHAR);
INSERT INTO classes VALUES (1, 1, 'A101');
INSERT INTO classes VALUES (2, 2, 'A102');
INSERT INTO classes VALUES (3, 3, 'A101');
SELECT s.name, c.room, sc.score FROM students s INNER JOIN classes c ON s.id = c.student_id INNER JOIN scores sc ON s.id = sc.student_id;
-- 7.8 自连接
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
-- 8.5 多聚合
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
-- 9.3 多列排序
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
-- 10.2 UNION (去重)
SELECT age FROM students WHERE age = 20 UNION SELECT age FROM students WHERE age = 21;
SELECT age FROM students WHERE age = 20 UNION SELECT age FROM students WHERE age = 20;
-- 10.3 UNION + ORDER BY
SELECT name FROM students WHERE age = 20 UNION ALL SELECT name FROM students WHERE age = 21;
-- 10.4 跨表 UNION
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
-- 12.2 IN 子查询 + WHERE
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
-- 13.3 事务内 UPDATE
BEGIN;
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
UPDATE accounts SET balance = balance + 100 WHERE id = 2;
SELECT * FROM accounts;
COMMIT;
SELECT * FROM accounts;
-- 13.4 事务内 ROLLBACK UPDATE
BEGIN;
UPDATE accounts SET balance = 0 WHERE id = 1;
SELECT * FROM accounts WHERE id = 1;
ROLLBACK;
SELECT * FROM accounts WHERE id = 1;
-- 13.5 事务内 DELETE
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
-- 14.3 EXPLAIN 聚合
EXPLAIN SELECT COUNT(*), AVG(salary) FROM employees;
EXPLAIN SELECT dept, COUNT(*) FROM employees GROUP BY dept;
-- 14.4 EXPLAIN 子查询
EXPLAIN SELECT * FROM students WHERE id IN (SELECT student_id FROM scores WHERE score > 90);
-- 15.1 空表查询
CREATE TABLE empty_table (id INT, val VARCHAR);
SELECT * FROM empty_table;
SELECT COUNT(*) FROM empty_table;
SELECT * FROM empty_table WHERE id = 1;
-- 15.2 NULL 处理
SELECT * FROM t1 WHERE name IS NULL;
SELECT * FROM t1 WHERE name IS NOT NULL;
SELECT * FROM nullable_test WHERE a IS NULL;
SELECT * FROM nullable_test WHERE a IS NOT NULL;
SELECT COALESCE(NULL, 1);
SELECT NULLIF(1, 1);
SELECT NULLIF(1, 2);
-- 15.3 大数值
INSERT INTO t2 VALUES (100, 999999, 999999.99);
INSERT INTO t2 VALUES (101, -999999, -999999.99);
SELECT * FROM t2 WHERE a >= 100;
-- 15.4 空字符串
INSERT INTO t1 VALUES (100, '');
SELECT * FROM t1 WHERE name = '';
SELECT * FROM t1 WHERE name LIKE '%';
-- 15.5 重复插入
INSERT INTO t1 VALUES (200, 'dup');
INSERT INTO t1 VALUES (200, 'dup2');
SELECT * FROM t1 WHERE id = 200;
-- 15.6 DELETE 后 INSERT
CREATE TABLE reuse_test (id INT PRIMARY KEY, val VARCHAR);
INSERT INTO reuse_test VALUES (1, 'first');
DELETE FROM reuse_test WHERE id = 1;
INSERT INTO reuse_test VALUES (1, 'second');
SELECT * FROM reuse_test;
-- 15.7 多次 UPDATE
CREATE TABLE multi_update (id INT, counter INT);
INSERT INTO multi_update VALUES (1, 0);
UPDATE multi_update SET counter = counter + 1 WHERE id = 1;
UPDATE multi_update SET counter = counter + 1 WHERE id = 1;
UPDATE multi_update SET counter = counter + 1 WHERE id = 1;
SELECT * FROM multi_update;
-- 15.8 GROUP BY 单行结果
SELECT dept, COUNT(*) FROM employees WHERE dept = 'Engineering' GROUP BY dept;
-- 15.9 HAVING 过滤所有组
SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 100;
-- 15.10 LIMIT 0
SELECT * FROM t1 LIMIT 0;
-- 15.11 OFFSET 超过行数
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
-- 16.5 子查询 + JOIN
SELECT s.name, sc.score FROM students s INNER JOIN scores sc ON s.id = sc.student_id WHERE s.id IN (SELECT student_id FROM scores WHERE score > 90);
-- 16.6 多层嵌套
SELECT * FROM employees WHERE dept IN ('Engineering', 'Sales') AND salary > (SELECT AVG(salary) FROM employees);
-- 16.7 表达式 + 聚合
SELECT dept, SUM(salary * 1.1) FROM employees GROUP BY dept;
SELECT dept, AVG(salary) - MIN(salary) AS salary_range FROM employees GROUP BY dept;
-- 16.8 CASE + 聚合
SELECT dept, SUM(CASE WHEN salary > 85000 THEN 1 ELSE 0 END) AS high_earners FROM employees GROUP BY dept;
-- 16.9 DISTINCT + ORDER BY
SELECT DISTINCT dept FROM employees ORDER BY dept ASC;
SELECT DISTINCT age FROM students ORDER BY age DESC;
-- 16.10 UNION + ORDER BY
SELECT name FROM students WHERE age = 20 UNION ALL SELECT name FROM students WHERE age = 21;
-- 17.1 建表 → 插入 → 查询 → 更新 → 删除 → 查询
CREATE TABLE lifecycle (id INT PRIMARY KEY, val VARCHAR);
INSERT INTO lifecycle VALUES (1, 'created');
SELECT * FROM lifecycle;
UPDATE lifecycle SET val = 'updated' WHERE id = 1;
SELECT * FROM lifecycle;
DELETE FROM lifecycle WHERE id = 1;
SELECT * FROM lifecycle;
DROP TABLE lifecycle;
-- 17.2 索引 + 查询
CREATE TABLE idx_test (id INT PRIMARY KEY, name VARCHAR, score INT);
CREATE INDEX idx_name ON idx_test (name);
INSERT INTO idx_test VALUES (1, 'Alice', 90);
INSERT INTO idx_test VALUES (2, 'Bob', 85);
INSERT INTO idx_test VALUES (3, 'Charlie', 95);
SELECT * FROM idx_test WHERE name = 'Alice';
EXPLAIN SELECT * FROM idx_test WHERE name = 'Alice';
-- 17.3 事务 + 索引
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
-- 18.5 混合类型比较
SELECT * FROM float_test WHERE f > 2 AND d < 4.0;
-- 19.1 批量插入
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
-- 19.2 全表扫描
SELECT * FROM perf_test;
SELECT COUNT(*) FROM perf_test;
SELECT AVG(val) FROM perf_test;
-- 19.3 范围查询
SELECT * FROM perf_test WHERE val BETWEEN 300 AND 700;
SELECT * FROM perf_test WHERE id > 5;
SELECT * FROM perf_test WHERE val < 500;
-- 19.4 排序
SELECT * FROM perf_test ORDER BY val DESC;
SELECT * FROM perf_test ORDER BY id ASC LIMIT 5;
-- 19.5 多次 UPDATE
UPDATE perf_test SET val = val + 1 WHERE id = 1;
UPDATE perf_test SET val = val + 1 WHERE id = 1;
UPDATE perf_test SET val = val + 1 WHERE id = 1;
SELECT * FROM perf_test WHERE id = 1;
-- 19.6 DELETE + INSERT 循环
DELETE FROM perf_test WHERE id = 10;
INSERT INTO perf_test VALUES (10, 1000);
SELECT * FROM perf_test WHERE id = 10;
-- 20.1 不存在的表
SELECT * FROM nonexistent;
INSERT INTO nonexistent VALUES (1);
-- 20.2 语法错误
SELECT FROM t1;
INSERT t1 VALUES (1);
CREATE t1;
-- 20.3 重复主键
CREATE TABLE dup_pk (id INT PRIMARY KEY, val VARCHAR);
INSERT INTO dup_pk VALUES (1, 'first');
INSERT INTO dup_pk VALUES (1, 'second');
-- 20.4 NOT NULL 违反
INSERT INTO t4 VALUES (100, NULL);
-- 20.5 类型不匹配 (应该能处理)
INSERT INTO t1 VALUES ('not_an_int', 'test');
-- 20.6 不支持的命令
ALTER TABLE t1 ADD COLUMN new_col INT;
SHOW DATABASES;
USE test;
-- 21.1 电商场景
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
-- 查询: 每个订单的商品详情
SELECT p.name, oi.quantity, p.price, oi.quantity * p.price AS total FROM products p INNER JOIN order_items oi ON p.id = oi.product_id;
-- 查询: 每个订单总金额
SELECT oi.order_id, SUM(oi.quantity * p.price) AS order_total FROM products p INNER JOIN order_items oi ON p.id = oi.product_id GROUP BY oi.order_id;
-- 查询: 每个品类的销售数量
SELECT p.category, SUM(oi.quantity) AS total_sold FROM products p INNER JOIN order_items oi ON p.id = oi.product_id GROUP BY p.category;
-- 查询: 最畅销商品 TOP 3
SELECT p.name, SUM(oi.quantity) AS sold FROM products p INNER JOIN order_items oi ON p.id = oi.product_id GROUP BY p.name ORDER BY sold DESC LIMIT 3;
-- 查询: 库存不足的商品
SELECT name, stock FROM products WHERE stock < 50;
-- 更新: 促销降价 10%
UPDATE products SET price = price * 0.9 WHERE category = 'Electronics';
SELECT * FROM products WHERE category = 'Electronics';
-- 21.2 学生成绩系统
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
-- GPA 计算
SELECT s.name, AVG(e.grade) AS gpa FROM students s INNER JOIN enrollments e ON s.id = e.student_id GROUP BY s.name ORDER BY gpa DESC;
-- 每门课平均分
SELECT c.name, AVG(e.grade) FROM courses c INNER JOIN enrollments e ON c.id = e.course_id GROUP BY c.name;
-- 不及格学生 (grade < 80)
SELECT s.name, c.name, e.grade FROM students s INNER JOIN enrollments e ON s.id = e.student_id INNER JOIN courses c ON e.course_id = c.id WHERE e.grade < 80;
-- 21.3 库存管理
CREATE TABLE inventory (id INT PRIMARY KEY, product VARCHAR, quantity INT, warehouse VARCHAR);
INSERT INTO inventory VALUES (1, 'Widget', 100, 'A');
INSERT INTO inventory VALUES (2, 'Gadget', 50, 'A');
INSERT INTO inventory VALUES (3, 'Widget', 200, 'B');
INSERT INTO inventory VALUES (4, 'Gadget', 75, 'B');
INSERT INTO inventory VALUES (5, 'Thingamajig', 30, 'A');
-- 每个仓库的总库存
SELECT warehouse, SUM(quantity) FROM inventory GROUP BY warehouse;
-- 每个产品的总库存
SELECT product, SUM(quantity) FROM inventory GROUP BY product ORDER BY SUM(quantity) DESC;
-- 库存低于 100 的产品
SELECT product, warehouse, quantity FROM inventory WHERE quantity < 100 ORDER BY quantity ASC;
-- 库存转移
BEGIN;
UPDATE inventory SET quantity = quantity - 20 WHERE product = 'Widget' AND warehouse = 'A';
UPDATE inventory SET quantity = quantity + 20 WHERE product = 'Widget' AND warehouse = 'B';
COMMIT;
SELECT * FROM inventory WHERE product = 'Widget';
-- 确保之前的修复仍然有效
-- 22.1 FLOAT 序列化
CREATE TABLE float_reg (id INT, val FLOAT);
INSERT INTO float_reg VALUES (1, 123.456);
INSERT INTO float_reg VALUES (2, 0.001);
INSERT INTO float_reg VALUES (3, -999.99);
SELECT * FROM float_reg;
-- 22.2 COUNT(*) 不崩溃
SELECT COUNT(*) FROM float_reg;
SELECT COUNT(*), AVG(val) FROM float_reg;
-- 22.3 多次 UPDATE + SELECT
CREATE TABLE update_reg (id INT, val INT);
INSERT INTO update_reg VALUES (1, 10);
UPDATE update_reg SET val = 20 WHERE id = 1;
UPDATE update_reg SET val = 30 WHERE id = 1;
UPDATE update_reg SET val = 40 WHERE id = 1;
SELECT * FROM update_reg WHERE id = 1;
-- 22.4 事务回滚
BEGIN;
INSERT INTO update_reg VALUES (2, 999);
ROLLBACK;
SELECT * FROM update_reg WHERE id = 2;
-- 22.5 DELETE + INSERT
DELETE FROM update_reg WHERE id = 1;
INSERT INTO update_reg VALUES (1, 100);
SELECT * FROM update_reg WHERE id = 1;
-- 22.6 SHOW TABLES 不崩溃
SHOW TABLES;
-- 22.7 DESC 不崩溃
DESC update_reg;
DESC float_reg;
-- 最终状态检查
SHOW TABLES;
SELECT COUNT(*) FROM t1;
SELECT COUNT(*) FROM employees;
SELECT COUNT(*) FROM students;
SELECT COUNT(*) FROM scores;
SELECT COUNT(*) FROM products;
SELECT COUNT(*) FROM accounts;
-- 清理
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
