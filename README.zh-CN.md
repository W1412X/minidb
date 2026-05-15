# MiniDB

MiniDB 是一个使用 C++20 实现的 PostgreSQL 风格关系型数据库内核，包含 MVCC 快照隔离、WAL 崩溃恢复、B+Tree 索引、代价优化器、分区 Buffer Pool、低内存降级执行路径，以及带准入控制的 TCP Server。

## 功能概览

### SQL 支持

| 类别 | 支持能力 |
|------|----------|
| DDL | `CREATE TABLE`、`DROP TABLE`、`ALTER TABLE`、`CREATE INDEX`、`CREATE UNIQUE INDEX`、`DROP INDEX` |
| DML | 多行 `INSERT`、带 `WHERE` 的 `UPDATE` / `DELETE` |
| 查询 | `SELECT`、`WHERE`、`INNER JOIN`、`LEFT JOIN`、`GROUP BY`、`HAVING`、`ORDER BY`、`LIMIT/OFFSET`、`DISTINCT`、`UNION/UNION ALL` |
| 表达式 | 算术、布尔表达式、`CASE WHEN`、`LIKE`、`BETWEEN`、`IS NULL`、`IN` / `NOT IN` |
| 子查询 | 标量子查询、`IN (SELECT ...)`、`NOT IN (SELECT ...)` |
| 聚合 | `COUNT`、`SUM`、`AVG`、`MIN`、`MAX` |
| 事务 | `BEGIN`、`COMMIT`、`ROLLBACK` |
| 预编译 | `PREPARE`、`EXECUTE`、`DEALLOCATE` |
| 管理 | `SHOW TABLES`、`DESCRIBE`、`EXPLAIN`、`ANALYZE`、`SHOW CONFIG`、`SHOW STATS` |
| 游标 | Server 模式下支持 `DECLARE CURSOR`、`FETCH`、`CLOSE` |

### 数据类型

支持以下字段类型和别名：

- `BOOL` / `BOOLEAN`
- `INT` / `INTEGER`
- `BIGINT`
- `FLOAT` / `REAL`
- `DOUBLE` / `DECIMAL` / `NUMERIC`
- `VARCHAR(n)`
- `TEXT`
- `NULL`

支持常见约束：

```sql
PRIMARY KEY
UNIQUE
NOT NULL
DEFAULT
```

## 存储引擎

- 8KB 页面，包含 slot array / line pointer 布局。
- Heap File 存储表数据，页级空闲空间跟踪。
- Buffer Pool 支持可配置大小、LRU、顺序扫描防污染，以及按 page id 分区的 page table / LRU 锁。
- B+Tree 索引支持单列索引、复合索引、唯一索引、等值查询、范围查询和 Index-Only Scan。
- Primary Key 和 Unique 字段会自动创建唯一索引。
- Double-write buffer 防止 torn page。
- Page checksum 用于检测静默损坏。
- FD cache 用于限制打开文件描述符数量。

## MVCC 与事务

- 快照隔离，每个事务看到一致快照。
- Tuple 包含 `xmin` / `xmax` 和版本链指针。
- 支持 HOT update，减少索引维护开销。
- 支持 undo record，事务回滚不依赖 REDO。
- GC 基于活跃事务水位线回收旧版本。
- UPDATE/COMMIT 路径会主动剪枝对所有活跃事务不可见的版本链。

## WAL 与恢复

- WAL-first：数据页刷盘前必须先写 WAL。
- WAL record 支持 Begin、Commit、Abort、Insert、Delete、Update、IndexInsert、IndexDelete、PageAlloc、Checkpoint。
- WAL 写入支持 8KB page buffer，攒页后批量写入/fsync。
- 支持 group commit，可配置批量提交延迟。
- 支持 checkpoint，按时间和 WAL 大小触发。
- 崩溃恢复会从 checkpoint 后重放 WAL。
- 索引恢复支持 lazy rebuild 或启动时重建。

## 查询执行器

MiniDB 使用 Volcano iterator 模型，主要执行器包括：

- `SeqScan`：顺序扫描、MVCC 可见性、版本链遍历、late materialization。
- `IndexScan`：B+Tree 等值和范围扫描。
- `IndexOnlyScan`：投影只需要索引 key 时避免回表。
- `Filter`：编译后的常见谓词快路径，失败时回退通用表达式求值。
- `Project`：列投影和表达式计算。
- `NestedLoopJoin`：小表或非等值连接。
- `HashJoin`：等值连接，小侧 build，低内存时 Grace Hash Join 分区 spill。
- `IndexLookupJoin`：外侧较小且内侧 join key 有索引时使用索引查找连接。
- `Sort`：内存排序、External Sort、Top-N heap。
- `Aggregate`：普通聚合、Hash Aggregate、spill 路径、`COUNT(*) JOIN` 快路径。
- `Distinct`：去重和 spill。
- `Limit`：`LIMIT/OFFSET`。
- `Union`：`UNION` 和 `UNION ALL`。
- `SubqueryIn`：`IN (SELECT ...)` / `NOT IN`。
- `Insert` / `Update` / `Delete`：维护 MVCC、索引和 WAL。

## 优化器

优化器采用规则 + 代价模型组合：

- 基于表行数、NDV、列统计估算选择率。
- 支持 `ANALYZE table;` 显式刷新统计信息。
- 自动选择 SeqScan、IndexScan、IndexOnlyScan。
- 支持等值和范围索引路径选择。
- 支持谓词下推：单表 `WHERE` 条件可下推到 JOIN 两侧。
- 支持投影下推：JOIN / COUNT 场景只读取必要列。
- 支持 `COUNT(*)` join-count 快路径，避免构造完整 join tuple。
- 支持 HashJoin 小侧 build 选择。
- 支持 IndexLookupJoin 选择。
- 支持索引有序输出消除多余 `ORDER BY` Sort。
- 支持 Top-N heap，`ORDER BY ... LIMIT N` 不需要全量排序。
- 内存不足时 HashJoin 使用 Grace hash partition，不退化为全量 nested-loop spill。

典型优化后计划：

```sql
EXPLAIN SELECT COUNT(*)
FROM orders o
JOIN order_items oi ON o.id = oi.order_id
WHERE o.id < 100;
```

如果 `orders.id` 和 `order_items.order_id` 有索引，优化器会倾向：

```text
Aggregate count-only
  IndexLookupJoin
    Filter / IndexScan orders(id < 100)
    Index-backed lookup order_items(order_id = outer.id)
```

## 快速开始

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j4
```

启动交互 Shell：

```bash
./build/minidb --dir ./mydata
```

启动 TCP Server：

```bash
./build/minidb --dir ./mydata --server --port 5433
```

查看有效配置：

```bash
./build/minidb --dir ./mydata --show-config
```

## SQL 示例

```sql
CREATE TABLE orders (
    id INT PRIMARY KEY,
    customer_id INT NOT NULL,
    total DOUBLE DEFAULT 0.0,
    status VARCHAR DEFAULT 'pending'
);

CREATE INDEX idx_orders_customer ON orders(customer_id);
CREATE INDEX idx_orders_status_total ON orders(status, total);

INSERT INTO orders VALUES
    (1, 100, 50.00, 'shipped'),
    (2, 101, 75.50, 'pending');

SELECT status, COUNT(*), SUM(total)
FROM orders
WHERE total > 10
GROUP BY status
ORDER BY SUM(total) DESC
LIMIT 10;

ANALYZE orders;
EXPLAIN SELECT * FROM orders WHERE customer_id = 100;
```

## 配置

配置文件位于：

```text
<db_dir>/minidb.conf
```

格式为 `key=value`，支持 `KB`、`MB`、`GB`、`MS`、`S`、`MIN` 等单位。

常用配置：

```ini
shared_buffers = 2MB
buffer_pool_partitions = 16
work_mem = 16MB
query_memory_limit = 512MB
temp_file_limit = 10GB
temp_dir = /tmp

wal_fsync = on
wal_group_commit = on
wal_group_commit_delay = 2ms
checkpoint_timeout = 60s
checkpoint_wal_size = 256MB

statement_timeout = 30s
enable_hashjoin = on
enable_indexscan = on
enable_indexonlyscan = on
enable_parallel_seqscan = on
parallel_workers = 4

gc_enabled = on
gc_ops_threshold = 10000

max_connections = 64
max_active_queries = 64
query_workers = 8
```

运行时查看：

```sql
SHOW CONFIG;
SHOW STATS;
```

## 测试

完整 CTest：

```bash
ctest --test-dir build --output-on-failure
```

项目聚合测试：

```bash
bash tests/run_all_tests.sh ./build/minidb
```

关键专项测试：

```bash
bash tests/sql_regression.sh ./build/minidb
bash tests/join_optimizer.sh ./build/minidb
bash tests/performance_optimizations.sh ./build/minidb
bash tests/resource_limits.sh ./build/minidb
python3 tests/consistency_test.py ./build/minidb
python3 tests/ultimate_test.py ./build/minidb
python3 tests/concurrent_test.py ./build/minidb
```

`tests/join_optimizer.sh` 覆盖：

- JOIN 谓词下推。
- JOIN 投影裁剪。
- HashJoin 小侧 build。
- Grace hash join spill。
- IndexLookupJoin。
- `COUNT(*) JOIN` 快路径。
- 多表 JOIN。
- `JOIN + WHERE`、`JOIN + GROUP BY`、`JOIN + ORDER BY LIMIT`、`LEFT JOIN + WHERE`、`JOIN + 子查询`。

## 目录结构

```text
src/
├── catalog/      # 表、索引和统计信息
├── common/       # 配置、锁、资源管理
├── concurrency/  # LockManager 和死锁检测
├── container/    # 自研容器
├── database/     # 数据库生命周期、统计、GC
├── index/        # B+Tree
├── network/      # TCP Server
├── record/       # Value、Tuple、Schema
├── recovery/     # WAL、checkpoint、double-write
├── repl/         # 交互 Shell
├── sql/executor/ # Volcano 执行器
├── sql/optimizer/# 优化器
├── sql/parser/   # Lexer、Parser、AST
├── sql/planner/  # 计划生成
├── storage/      # BufferPool、DiskManager、HeapFile、Page
└── transaction/  # MVCC 事务管理

tests/
├── run_all_tests.sh
├── sql_regression.sh
├── join_optimizer.sh
├── performance_optimizations.sh
├── consistency_test.py
├── ultimate_test.py
└── concurrent_test.py
```

## 运行要求

- C++20 编译器。
- CMake 3.20+。
- Python 3.8+。
- POSIX 系统，Linux 或 macOS。

## License

MIT
