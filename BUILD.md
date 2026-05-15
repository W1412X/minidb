# Build & Usage Guide

## Prerequisites

- **C++17** compiler (g++ 7+, clang 6+, Apple Clang 12+)
- **CMake** 3.10 or later
- **Python 3.8+** (for test scripts and data loading)
- **POSIX** system (Linux, macOS — requires pthreads, sockets, mmap)

## Build

### Release Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### Debug Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

### Build Output

```
build/
├── minidb                    # Main binary (shell + server)
├── libminidb_lib.a           # Static library
└── tests/
    ├── lock_manager_wait_test  # Lock manager unit test
    └── tuple_value_edge_test   # Value/tuple edge case test
```

### Clean Build

```bash
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

---

## Interactive Shell

```bash
./build/minidb --dir ./mydata
```

This starts an interactive REPL. Type SQL statements terminated by `;`. Special commands:

| Command | Description |
|---------|-------------|
| `exit` | Quit the shell |
| `help` | Show help message |

Example session:

```
Data directory: ./mydata
minidb> CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR);
Table 'users' created.

minidb> INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');
affected_rows
2

minidb> SELECT * FROM users;
id | name
1  | Alice
2  | Bob

minidb> SELECT COUNT(*) FROM users;
agg_0
2

minidb> exit
```

---

## TCP Server

### Start Server

```bash
# Default port (5433)
./build/minidb --dir ./mydata --server

# Custom port
./build/minidb --dir ./mydata --server --port 5433
```

Output:
```
MiniADB v0.3.0 — Server Mode
Data directory: ./mydata
```

### Connect via netcat

```bash
nc localhost 5433
```

```
MiniADB v0.3.0 — Connected.
Type SQL statements ending with ;
minidb> SELECT COUNT(*) FROM orders;
agg_0
1000
minidb> exit
```

### Connect via Python

```python
import socket

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", 5433))

# Read welcome banner
banner = s.recv(4096)
print(banner.decode())

# Send SQL
s.sendall(b"SELECT COUNT(*) FROM orders;\n")

# Read response
response = s.recv(4096)
print(response.decode())

s.sendall(b"exit\n")
s.close()
```

### Connect via Shell Script

```bash
echo -e "SELECT COUNT(*) FROM orders;\nexit\n" | nc localhost 5433
```

### Stop Server

Press `Ctrl+C` to gracefully shut down. Data is automatically saved (WAL flush, checkpoint).

---

## Configuration

### Configuration File

Place `minidb.conf` in the database directory `<db_dir>/minidb.conf`, or specify a custom path with `--config`:

```bash
./build/minidb --dir ./mydata --config /path/to/minidb.conf
```

### File Format

```
# Lines starting with # are comments
key = value
```

**Value units supported:**
- Memory: `B`, `KB`, `MB`, `GB`
- Time: `MS`, `S`, `MIN`
- Percent: `%` (e.g., `70%`)
- Boolean: `on`/`off`, `true`/`false`, `1`/`0`

### Full Configuration Reference

#### Memory

| Key | Default | Description |
|-----|---------|-------------|
| `shared_buffers` | 2MB | Buffer pool size (default 2MB = 256 pages × 8KB). Larger = more cached data, faster queries, more RAM usage. |
| `work_mem` | 16MB | Per-query working memory for sort operations, hash tables, and aggregation. Queries exceeding this spill to temp files. |
| `query_memory_limit` | 512MB | Maximum memory any single query can consume before being cancelled. |
| `maintenance_work_mem` | 256MB | Memory for maintenance operations (index rebuild, statistics collection). |
| `temp_file_limit` | 10GB | Maximum temp file space a query can use for spilled sort/hash/aggregate data. |
| `temp_dir` | /tmp | Directory for temp files (spilled sort runs, hash partitions). |
| `max_result_rows` | 1000000 | Maximum number of rows in a query result. |
| `max_result_bytes` | 256MB | Maximum size of a query result in bytes. |

#### WAL & Crash Recovery

| Key | Default | Description |
|-----|---------|-------------|
| `wal_segment_size` | 64MB | Size of each WAL segment file. Larger = fewer files, more data lost on crash. |
| `wal_keep_segments` | 2 | Number of old WAL segments to retain after checkpoint. |
| `wal_fsync` | on | Whether to fsync WAL writes. `off` gives higher throughput but risks data loss on crash. |
| `wal_group_commit` | on | Batch multiple commit requests into a single fsync. |
| `wal_group_commit_delay` | 2ms | Maximum delay to wait for more commits when group commit is enabled. |
| `checkpoint_timeout` | 60s | Time between automatic checkpoints. |
| `checkpoint_wal_size` | 256MB | WAL size that triggers an automatic checkpoint. |
| `recover_indexes` | lazy | `lazy` = rebuild indexes on first use (faster startup). `rebuild` = rebuild all indexes during recovery (faster first query). |
| `recovery_parallelism` | 4 | Number of threads used during crash recovery. |
| `startup_scan_txn_watermark` | off | Scan transaction watermark at startup for GC (slower startup, more aggressive cleanup). |
| `doublewrite` | on | Double-write buffer protection against torn pages. |
| `page_checksum` | on | Page checksums for detecting data corruption. |

#### Query Execution

| Key | Default | Description |
|-----|---------|-------------|
| `statement_timeout` | 30s | Maximum execution time for a single query. |
| `enable_hashjoin` | on | Allow hash join execution. |
| `enable_indexscan` | on | Allow index scan execution. |
| `enable_parallel_seqscan` | on | Allow parallel sequential scan. |
| `parallel_workers` | 4 | Number of worker threads for parallel operations. |
| `seqscan_prefetch_pages` | 32 | Number of pages to prefetch in sequential scan. |

#### Garbage Collection

| Key | Default | Description |
|-----|---------|-------------|
| `gc_enabled` | on | Enable MVCC garbage collection. |
| `gc_ops_threshold` | 10000 | Number of operations (inserts/updates/deletes) before GC runs. |
| `gc_max_pages_per_cycle` | 128 | Maximum pages scanned per GC cycle. |
| `gc_interval` | 5s | Interval between GC checks. |
| `deleted_tuple_ratio_threshold` | 20% | Ratio of dead tuples that triggers GC on a table. |

#### Network / Server

| Key | Default | Description |
|-----|---------|-------------|
| `listen_addresses` | 127.0.0.1 | IP address to bind the server to. Use `0.0.0.0` for all interfaces. |
| `port` | 5433 | TCP port for the server. |
| `max_connections` | 64 | Maximum concurrent TCP connections. |
| `max_active_queries` | 64 | Maximum concurrently executing queries (queued if exceeded). |
| `max_active_write_queries` | 8 | Maximum concurrent write queries (INSERT/UPDATE/DELETE). |
| `max_active_transactions` | 256 | Maximum concurrent active transactions. |
| `admission_queue_size` | 1024 | Maximum number of queries waiting in admission queue. |
| `admission_queue_timeout` | 5s | Time a query waits in admission queue before being rejected. |
| `transaction_slot_wait_timeout` | 5s | Time a transaction waits for a slot before failing. |
| `query_workers` | 8 | Number of worker threads for executing queries. |
| `io_workers` | 2 | Number of I/O worker threads. |
| `connection_idle_timeout` | 5min | Time before an idle connection is closed. |
| `client_output_buffer_limit` | 16MB | Maximum output buffer per client before being disconnected. |
| `buffer_pool_wait_timeout` | 5s | Time a query waits for a buffer pool slot before erroring. |
| `max_buffer_waiters` | 1024 | Maximum threads waiting for buffer pool slots. |
| `dirty_page_threshold` | 70% | Percentage of dirty pages that triggers background flush. |
| `background_flush_pages` | 64 | Number of pages flushed per background flush cycle. |
| `checkpoint_flush_after` | 128 | Number of pages to flush before a checkpoint. |
| `max_sql_size` | 1MB | Maximum SQL text length. |
| `tcp_keepalive` | on | Enable TCP keepalive on client connections. |
| `fd_cache_limit` | 1024 | Maximum cached file descriptors. |

### Example Configuration File

```ini
# Memory
shared_buffers = 64MB
work_mem = 32MB
query_memory_limit = 1GB
temp_file_limit = 2GB
temp_dir = /tmp/minidb_temp

# Server
port = 5433
max_connections = 128
max_active_queries = 32
max_active_write_queries = 16
query_workers = 16
connection_idle_timeout = 10min

# WAL
wal_segment_size = 128MB
wal_fsync = on
wal_group_commit = on
wal_group_commit_delay = 1ms
checkpoint_timeout = 120s
checkpoint_wal_size = 512MB

# Performance
work_mem = 64MB
enable_hashjoin = on
enable_indexscan = on
statement_timeout = 60s

# GC
gc_enabled = on
gc_interval = 10s
```

---

## Data Loading

### Load Script

```bash
# Small smoke test (1K rows)
python3 scripts/load_perf_data.py \
    --bin ./build/minidb \
    --dir ./mydata \
    --preset smoke \
    --clean

# Full dataset (1M+ rows across 8 tables)
python3 scripts/load_perf_data.py \
    --bin ./build/minidb \
    --dir ./mydata \
    --preset full \
    --clean

# Custom scale
python3 scripts/load_perf_data.py \
    --bin ./build/minidb \
    --dir ./mydata \
    --preset full \
    --customers 50000 \
    --orders 500000 \
    --items 600000 \
    --clean
```

### Manual Data Loading

```bash
# Through shell (for small datasets)
./build/minidb --dir ./mydata << 'EOF'
CREATE TABLE test (id INT PRIMARY KEY, val VARCHAR);
INSERT INTO test VALUES (1, 'hello'), (2, 'world');
EOF
```

---

## Running Tests

### All Tests (400+)

```bash
bash tests/run_all_tests.sh ./build/minidb
```

This runs 9 suites sequentially and reports pass/fail.

### Individual Test Suites

| Suite | Command | Tests | Coverage |
|-------|---------|-------|----------|
| Comprehensive | `python3 tests/comprehensive_test.py ./build/minidb` | 191 | SQL variants, edge cases |
| Consistency | `python3 tests/consistency_test.py ./build/minidb` | 62 | Persistence, crash recovery |
| Ultimate | `python3 tests/ultimate_test.py ./build/minidb` | 105 | All-combination edge cases |
| Concurrent | `python3 tests/concurrent_test.py ./build/minidb` | 8 | Server concurrency |
| Bug Verify | `python3 tests/bug_verify.py ./build/minidb` | 32 | Regression tests |
| SQL Regression | `bash tests/sql_regression.sh ./build/minidb` | ~60 | SQL syntax + execution |
| Resource Limits | `bash tests/resource_limits.sh ./build/minidb` | ~15 | Resource management |
| Recovery Smoke | `bash tests/recovery_smoke.sh ./build/minidb` | ~10 | Crash recovery |
| Performance Paths | `bash tests/performance_paths.sh ./build/minidb` | ~10 | Execution paths |

### Unit Tests

```bash
# Lock manager deadlock detection + wait
./build/tests/lock_manager_wait_test

# Tuple + Value edge cases (serialization, nulls, types)
./build/tests/tuple_value_edge_test
```

### Performance Benchmarks

```bash
# Single-operation latency
python3 tests/perf_test.py ./build/minidb

# Large-scale (1M+ rows, may take hours)
bash tests/large_scale_benchmark.py ./build/minidb
```

---

## Data Directory Layout

```
./mydata/
├── minidb.conf          # Configuration (auto-created, optional)
├── catalog.mdbc         # Serialized system catalog
├── doublewrite.bin      # Double-write buffer (torn-page protection)
├── wal/                 # WAL segments
│   ├── wal.00000001.log
│   └── ...
├── tables/              # Heap files (one per table)
│   ├── 1.heap
│   ├── 2.heap
│   └── ...
├── indexes/             # B+ Tree files (one per index)
│   ├── 1000.btree
│   ├── 1001.btree
│   └── ...
└── tmp/                 # Temp files (spilled data)
```

Clear all data to start fresh:
```bash
rm -rf ./mydata
```

Note: The WAL directory must exist for the server to start. If using `--clean` with `load_perf_data.py`, this is handled automatically.

---

## Performance Tuning Guidelines

### For High Throughput

```ini
shared_buffers = 25% of available RAM    # More cache = fewer disk reads
wal_fsync = off                           # Disable fsync for maximum throughput
temp_dir = /path/to/fast/ssd              # Fast temp storage for spills
work_mem = 64MB                           # Reduce spill for sort/hash
```

### For Memory-Constrained Environments

```ini
shared_buffers = 1MB                      # Minimum buffer pool
work_mem = 1MB                            # Aggressive spill at low memory
temp_file_limit = 256MB                   # Limit temp usage
max_connections = 16                      # Reduce concurrency
max_active_queries = 4                    # Limit active queries
query_memory_limit = 128MB                # Cap per-query memory
```

### For Maximum Durability

```ini
wal_fsync = on                            # Guarantee WAL durability
wal_group_commit = on                     # Still efficient with batching
doublewrite = on                          # Torn-page protection
page_checksum = on                        # Corruption detection
checkpoint_timeout = 30s                  # Frequent checkpoints
```

---

## Troubleshooting

### Server Won't Start

```bash
# Check if port is already in use
lsof -i :5433

# Check config file syntax
./build/minidb --show-config --dir ./mydata

# Clear corrupt data
rm -rf ./mydata && mkdir -p ./mydata/wal
```

### WAL Corruption on Startup

If the server hangs on startup with WAL corruption:
```bash
rm -rf ./mydata/wal/
```

### Out of Memory

Reduce memory settings and restart:
```ini
shared_buffers = 1MB
work_mem = 1MB
max_connections = 8
```

### Process Leaves Stale Data

The double-write buffer and WAL are cleaned on clean shutdown. After a crash:
```bash
rm -f ./mydata/doublewrite.bin
```

---

## License

MIT
