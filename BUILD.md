# Build And Usage Guide

## Prerequisites

- C++20 compiler.
- CMake 3.20 or later.
- Python 3.8+ for test scripts and data loading.
- POSIX system such as Linux or macOS.

## Build

```bash
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j4
```

Debug build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build build -j4
```

Build outputs:

```text
build/
├── minidb                    # Interactive shell and SQL TCP server
├── minidb_pageserver         # Standalone PageServer process
├── libminidb_lib.a
└── tests/
    ├── lock_manager_wait_test
    ├── tuple_value_edge_test
    └── page_store_remote_test
```

## Single-Node Shell

```bash
./build/minidb --dir ./mydata
```

Example:

```sql
CREATE TABLE users (id INT PRIMARY KEY, name TEXT);
INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');
SELECT COUNT(*) FROM users;
EXPLAIN ANALYZE SELECT * FROM users WHERE id = 1;
```

## SQL TCP Server

```bash
./build/minidb --dir ./mydata --server --port 5433
nc 127.0.0.1 5433
```

The SQL TCP server supports normal SQL execution, `SHOW CONFIG`, `SHOW STATS`, prepared statements, and server-side cursor commands.

## Standalone PageServer Mode

Start PageServer:

```bash
mkdir -p ./pageserver-data ./compute-data
./build/minidb_pageserver --dir ./pageserver-data --host 127.0.0.1 --port 15433
```

Create compute config:

```bash
cat > ./compute-data/minidb.conf <<'EOF'
storage_mode = remote
page_server_host = 127.0.0.1
page_server_port = 15433
remote_page_batch_size = 64
remote_flush_batch_size = 64
remote_connect_timeout = 1s
remote_io_timeout = 5s
remote_retry_count = 2
EOF
```

Start compute:

```bash
./build/minidb --dir ./compute-data --config ./compute-data/minidb.conf
```

Remote mode currently supports a single writer compute and read-only snapshot reads over remote page storage. It does not implement Raft, automatic failover, distributed locks, or multi-writer distributed transactions.

## Configuration

Configuration files are `key=value` files. Comments start with `#`.

Supported units:

- Memory: `B`, `KB`, `MB`, `GB`
- Time: `MS`, `S`, `MIN`
- Percent: `%`
- Boolean: `on`/`off`, `true`/`false`, `1`/`0`

Common settings:

| Key | Default | Implemented use |
| --- | --- | --- |
| `shared_buffers` | 2MB | Buffer pool size |
| `work_mem` | 16MB | Sort/hash/aggregate memory before spill |
| `query_memory_limit` | 512MB | Query resource admission/memory cap |
| `temp_file_limit` | 10GB | Spill temp-file cap |
| `temp_dir` | `/tmp` | Spill file directory |
| `statement_timeout` | 30s | Executor deadline |
| `enable_hashjoin` | on | Optimizer join selection |
| `enable_indexscan` | on | Optimizer scan path selection |
| `enable_indexonlyscan` | on | Optimizer index-only scan selection |
| `enable_parallel_seqscan` | on | Enables parallel seq scan for larger scans |
| `parallel_workers` | 4 | Parallel seq scan workers |
| `wal_fsync` | on | WAL fsync policy |
| `wal_group_commit` | on | Commit batching |
| `wal_group_commit_delay` | 2ms | Max group-commit delay |
| `checkpoint_timeout` | 60s | Background checkpoint interval |
| `checkpoint_wal_size` | 256MB | WAL bytes before checkpoint |
| `gc_enabled` | on | MVCC GC toggle |
| `gc_ops_threshold` | 10000 | Operation threshold before GC |
| `gc_max_pages_per_cycle` | 128 | GC scan cap per cycle |
| `max_connections` | 64 | SQL server connection limit |
| `max_active_queries` | 64 | Query admission limit |
| `max_active_write_queries` | 8 | Write-query admission limit |
| `max_active_transactions` | 256 | Transaction slot cap |
| `query_workers` | 8 | SQL server worker count |
| `buffer_pool_wait_timeout` | 5s | Wait time for buffer pool slots |
| `max_buffer_waiters` | 1024 | Buffer slot wait queue cap |
| `doublewrite` | on | Double-write page protection |
| `page_checksum` | on | Page checksum validation |
| `fd_cache_limit` | 1024 | DiskManager file descriptor cache cap |

Remote PageServer settings:

| Key | Default | Implemented use |
| --- | --- | --- |
| `storage_mode` | local | `local` or `remote` PageStore |
| `page_server_host` | empty | If set, compute uses TCP PageServer |
| `page_server_port` | 15433 | TCP PageServer port |
| `page_server_dir` | empty | In-process PageServer data dir when host is empty |
| `storage_read_only` | off | RO compute mode |
| `storage_read_lsn` | 0 | RO snapshot LSN |
| `page_server_replicas` | 0 | Local synchronous replica directories |
| `remote_page_batch_size` | 64 | TCP batch chunk size |
| `remote_flush_batch_size` | 64 | BufferPool dirty-page flush batch size |
| `remote_connect_timeout` | 1s | TCP connect timeout |
| `remote_io_timeout` | 5s | TCP send/receive timeout |
| `remote_retry_count` | 2 | RPC retry attempts |
| `remote_max_connections` | 8 | Client idle connection pool limit |
| `page_server_max_connections` | 1024 | PageServer active connection cap |

Inspect effective config:

```bash
./build/minidb --dir ./mydata --show-config
```

At runtime:

```sql
SHOW CONFIG;
SHOW STATS;
```

## Tests

Run all CTest targets:

```bash
ctest --test-dir build --output-on-failure
```

Run the project test runner:

```bash
bash tests/run_all_tests.sh ./build/minidb
```

Targeted tests:

```bash
./build/tests/lock_manager_wait_test
./build/tests/tuple_value_edge_test
./build/tests/page_store_remote_test
bash tests/remote_page_store.sh ./build/minidb
bash tests/sql_regression.sh ./build/minidb
bash tests/join_optimizer.sh ./build/minidb
bash tests/performance_optimizations.sh ./build/minidb
bash tests/recovery_smoke.sh ./build/minidb
bash tests/resource_limits.sh ./build/minidb
```

## Data Directory Layout

Single-node or compute directory:

```text
mydata/
├── catalog.mdbc
├── minidb.control
├── doublewrite.bin
├── wal/
├── tables/
├── indexes/
└── minidb.conf
```

Standalone PageServer directory:

```text
pageserver-data/
├── page_server.meta
├── remote_wal_images.bin
├── doublewrite.bin
├── tables/
├── indexes/
└── replica_1/              # if page_server_replicas >= 1
```

## Troubleshooting

Check SQL server port:

```bash
lsof -i :5433
```

Check PageServer port:

```bash
lsof -i :15433
```

Check config parsing:

```bash
./build/minidb --dir ./mydata --show-config
```

For a clean local test:

```bash
rm -rf ./mydata ./compute-data ./pageserver-data
```

## License

MIT
