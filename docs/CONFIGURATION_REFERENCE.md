# MiniDB Configuration Reference

Complete reference for the `DbConfig` struct (`src/common/db_config.h`) and
compile-time constants (`src/common/config.h`).

All runtime parameters can be set in a configuration file loaded by
`DbConfigLoader::load_file()`. The file format is `key = value`, one per line.
Lines starting with `#` are comments. Keys are **case-sensitive**; values for
boolean parameters accept `on`/`off`, `true`/`false`, or `1`/`0`.

Numeric values support unit suffixes:

| Suffix | Meaning              |
|--------|----------------------|
| `B`    | bytes                |
| `KB`   | kibibytes (x 1024)   |
| `MB`   | mebibytes (x 1024^2) |
| `GB`   | gibibytes (x 1024^3) |
| `MS`   | milliseconds         |
| `S`    | seconds (x 1000 ms)  |
| `MIN`  | minutes (x 60000 ms) |

Percentage parameters accept an optional trailing `%` (e.g. `85%` or `85`).

---

## Memory

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `shared_buffers_bytes` | `shared_buffers` | u64 | 2 MB (`kDefaultPoolFrames` x `kPageSize` = 256 x 8192) | Total buffer pool memory. Determines how many page frames the database keeps in RAM. |
| `work_mem_bytes` | `work_mem` | u64 | 16 MB | Per-operator memory budget for sort, hash, and aggregate operators before they spill to disk. |
| `query_memory_limit` | `query_memory_limit` | u64 | 512 MB | Maximum memory a single query execution can consume. |
| `maintenance_work_mem_bytes` | `maintenance_work_mem` | u64 | 256 MB | Memory available for maintenance operations such as index rebuilds and ANALYZE. |
| `memory_pressure_threshold_percent` | `memory_pressure_threshold` | u32 | 85 | Percentage of `query_memory_limit` at which memory-pressure handling is activated. Accepts 0-100. |

---

## Temp Files

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `temp_file_limit_bytes` | `temp_file_limit` | u64 | 10 GB | Maximum total temporary file space across all queries. |
| `temp_dir` | `temp_dir` | String | `"/tmp"` | Directory for spill / temporary files. |

---

## Results

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `max_result_rows` | `max_result_rows` | u64 | 1,000,000 | Maximum number of rows a single query may return. |
| `max_result_bytes` | `max_result_bytes` | u64 | 256 MB | Maximum byte size of a single query result. |

---

## Recovery

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `recovery_parallelism` | `recovery_parallelism` | u32 | 4 | Number of parallel workers used during WAL recovery. |
| `recover_indexes_lazy` | `recover_indexes` | bool | true | When **true** (config value is anything other than `rebuild`), indexes are rebuilt lazily after WAL recovery. Set the config-file key to `rebuild` to force eager index reconstruction. |
| `startup_scan_txn_watermark` | `startup_scan_txn_watermark` | bool | false | When enabled, scans storage for the transaction-id watermark on startup. |
| `consistency_check_on_startup` | `consistency_check_on_startup` | bool | false | Runs `check_table_index_consistency()` after WAL recovery and refuses to open the database if heap/index disagreement is found. **Note:** the check is O(N) over all heap rows; enable it only when silent corruption is a worse outcome than a longer startup. |

---

## Checkpoint

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `checkpoint_timeout_ms` | `checkpoint_timeout` | u64 | 60,000 (1 min) | Time-based checkpoint trigger interval. A checkpoint is initiated if this interval elapses since the last one. |
| `checkpoint_wal_size_bytes` | `checkpoint_wal_size` | u64 | 256 MB | WAL size threshold that triggers a checkpoint. |

---

## WAL (Write-Ahead Log)

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `wal_segment_size_bytes` | `wal_segment_size` | u64 | 64 MB | Size of each WAL segment file. |
| `wal_keep_segments` | `wal_keep_segments` | u32 | 2 | Number of old WAL segments retained after checkpoint. |
| `wal_fsync` | `wal_fsync` | bool | true | Whether WAL writes are fsynced to durable storage. Disabling improves throughput but risks data loss on crash. |
| `wal_group_commit` | `wal_group_commit` | bool | true | Enable group commit optimization, batching multiple transaction commits into a single WAL flush. |
| `wal_group_commit_delay_ms` | `wal_group_commit_delay` | u64 | 2 | Maximum delay (in ms) the group commit leader waits to collect more commits before flushing. |

---

## Query Execution

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `statement_timeout_ms` | `statement_timeout` | u64 | 30,000 (30 s) | Maximum wall-clock time a single query may execute before being cancelled. |
| `enable_hashjoin` | `enable_hashjoin` | bool | true | Allow the optimizer to choose hash join plans. |
| `enable_indexscan` | `enable_indexscan` | bool | true | Allow the optimizer to choose index scan plans. |
| `enable_indexonlyscan` | `enable_indexonlyscan` | bool | true | Allow the optimizer to choose index-only scan plans. |
| `enable_parallel_seqscan` | `enable_parallel_seqscan` | bool | true | Allow parallel sequential scans. |
| `parallel_workers` | `parallel_workers` | u32 | 4 | Number of worker threads for parallel scan operators. |
| `seqscan_prefetch_pages` | `seqscan_prefetch_pages` | u32 | 32 | Number of pages to prefetch ahead during a sequential scan. |

---

## Garbage Collection

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `gc_enabled` | `gc_enabled` | bool | true | Enable background garbage collection of dead tuples. |
| `gc_ops_threshold` | `gc_ops_threshold` | u32 | 10,000 | Number of DML operations accumulated before a GC cycle is triggered. |
| `gc_max_pages_per_cycle` | `gc_max_pages_per_cycle` | u32 | 128 | Maximum heap pages processed in a single GC cycle. |
| `gc_interval_ms` | `gc_interval` | u64 | 5,000 (5 s) | Minimum wall-clock interval between successive GC runs. |
| `deleted_tuple_ratio_threshold_percent` | `deleted_tuple_ratio_threshold` | u32 | 20 | Per-table dead-tuple ratio (%) that makes the table eligible for GC. Accepts 0-100. |

---

## Network

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `listen_addresses` | `listen_addresses` | String | `"127.0.0.1"` | TCP address(es) the server listens on. |
| `port` | `port` | u16 | 5433 (`kDefaultPort`) | TCP port the server listens on. |
| `max_connections` | `max_connections` | u32 | 64 (`kMaxConnections`) | Maximum number of concurrent client connections. |
| `max_active_queries` | `max_active_queries` | u32 | 64 | Maximum number of concurrently executing queries. |
| `max_active_write_queries` | `max_active_write_queries` | u32 | 8 | Maximum number of concurrently executing write (DML/DDL) queries. |
| `max_active_transactions` | `max_active_transactions` | u32 | 256 | Maximum number of concurrently active transactions. |
| `admission_queue_size` | `admission_queue_size` | u32 | 1,024 | Size of the admission-control queue for queries awaiting execution slots. |
| `admission_queue_timeout_ms` | `admission_queue_timeout` | u64 | 5,000 | Timeout (ms) for a query waiting in the admission queue. |
| `transaction_slot_wait_timeout_ms` | `transaction_slot_wait_timeout` | u64 | 5,000 | Timeout (ms) for a transaction waiting for a free transaction slot. |
| `query_workers` | `query_workers` | u32 | 8 | Number of worker threads dedicated to query processing. |
| `io_workers` | `io_workers` | u32 | 2 | Number of worker threads for background I/O tasks. |
| `connection_idle_timeout_ms` | `connection_idle_timeout` | u64 | 300,000 (5 min) | Time after which an idle connection is closed. |
| `client_output_buffer_limit_bytes` | `client_output_buffer_limit` | u64 | 16 MB | Maximum output buffer size per client connection. |
| `max_sql_size` | `max_sql_size` | u32 | 1 MB (1,048,576) | Maximum allowed size of a single SQL statement in bytes. |
| `tcp_keepalive` | `tcp_keepalive` | bool | true | Enable TCP keepalive probes on client connections. |

---

## Buffer Pool

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `buffer_pool_wait_timeout_ms` | `buffer_pool_wait_timeout` | u64 | 5,000 | Timeout (ms) for a thread waiting to acquire a buffer pool frame. |
| `max_buffer_waiters` | `max_buffer_waiters` | u32 | 1,024 | Maximum number of threads that may simultaneously wait for a free buffer pool frame. |
| `buffer_pool_partitions` | `buffer_pool_partitions` | u32 | 16 | Number of partitions (shards) in the buffer pool to reduce lock contention. |
| `dirty_page_threshold_percent` | `dirty_page_threshold` | u32 | 70 | When the ratio of dirty pages exceeds this percentage, background flushing is triggered. Accepts 0-100. |
| `background_flush_pages` | `background_flush_pages` | u32 | 64 | Number of dirty pages flushed per background-flush cycle. |
| `checkpoint_flush_after` | `checkpoint_flush_after` | u32 | 128 | Number of pages flushed between checkpoint progress updates. |

---

## Storage Integrity

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `doublewrite` | `doublewrite` | bool | true | Enable the doublewrite buffer to protect against torn (partial) page writes. |
| `page_checksum` | `page_checksum` | bool | true | Enable checksums on page bodies to detect silent corruption. |
| `fd_cache_limit` | `fd_cache_limit` | u32 | 1,024 | Maximum number of file descriptors kept in the FD cache. |

---

## Compute-Storage Separation

These parameters configure MiniDB's disaggregated compute-storage architecture
with the PageServer.

| Parameter | Config-file key | Type | Default | Description |
|-----------|----------------|------|---------|-------------|
| `storage_mode` | `storage_mode` | String | `"local"` | Storage mode. `"local"` uses the co-located storage engine; `"remote"` delegates page I/O to a remote PageServer. |
| `page_server_dir` | `page_server_dir` | String | `""` (empty) | Directory for local PageServer storage. When empty, defaults to `<db_dir>/page_server`. |
| `page_server_host` | `page_server_host` | String | `""` (empty) | TCP host of the remote PageServer (used when `storage_mode = remote`). |
| `page_server_port` | `page_server_port` | u16 | 15433 | TCP port of the remote PageServer. |
| `storage_read_only` | `storage_read_only` | bool | false | When true, the compute node operates in read-only mode and does not generate WAL. |
| `storage_read_lsn` | `storage_read_lsn` | u64 | 0 | Fixed LSN used for page reads in read-only compute mode. 0 means "latest". |
| `page_server_replicas` | `page_server_replicas` | u32 | 0 | Number of local PageServer replica directories for redundancy. |
| `remote_page_batch_size` | `remote_page_batch_size` | u32 | 64 | Number of pages fetched per batch in remote read operations. Minimum clamped to 1. |
| `remote_flush_batch_size` | `remote_flush_batch_size` | u32 | 64 | Number of pages flushed per batch in remote write operations. Minimum clamped to 1. |
| `remote_connect_timeout_ms` | `remote_connect_timeout` | u32 | 1,000 | Timeout (ms) for establishing a connection to the remote PageServer. Minimum clamped to 1. |
| `remote_io_timeout_ms` | `remote_io_timeout` | u32 | 5,000 | Timeout (ms) for a single remote I/O operation. Minimum clamped to 1. |
| `remote_retry_count` | `remote_retry_count` | u32 | 2 | Number of retries for failed remote operations before returning an error. |
| `remote_max_connections` | `remote_max_connections` | u32 | 8 | Maximum number of TCP connections from a compute node to the remote PageServer. Minimum clamped to 1. |
| `page_server_max_connections` | `page_server_max_connections` | u32 | 1,024 | Maximum number of connections the PageServer will accept. Minimum clamped to 1. |
| `page_server_cached_versions_per_page` | `page_server_cached_versions_per_page` | u32 | 32 | Number of historical page versions cached per page in the PageServer (for MVCC reads at past LSNs). Minimum clamped to 1. |

---

## Compile-Time Constants

Defined in `src/common/config.h`. These are `constexpr` values and cannot be
changed at runtime.

### Storage

| Constant | Type | Value | Description |
|----------|------|-------|-------------|
| `kPageSize` | u32 | 8,192 (8 KB) | Database page size in bytes. |
| `kPageHeaderSize` | u32 | 24 | Bytes reserved for the page header. |
| `kLinePointerSize` | u32 | 6 | Size of a line pointer entry: offset (2) + length (2) + flags (2). |
| `kPageTailReserved` | u32 | 8 | Bytes reserved at the page tail for `next_page_id`. |

### Buffer Pool

| Constant | Type | Value | Description |
|----------|------|-------|-------------|
| `kDefaultPoolFrames` | u32 | 256 | Default number of buffer pool frames (yields 2 MB with 8 KB pages). |
| `kMaxPoolFrames` | u32 | 65,536 | Maximum number of buffer pool frames (yields 512 MB with 8 KB pages). |

### B+ Tree

| Constant | Type | Value | Description |
|----------|------|-------|-------------|
| `kBTreeOrder` | u32 | 128 | B+ tree order (maximum children per internal node). |
| `kMaxKeysPerNode` | u32 | 127 | Maximum keys per B+ tree node (`kBTreeOrder - 1`). |

### WAL

| Constant | Type | Value | Description |
|----------|------|-------|-------------|
| `kWalBufferSize` | u32 | 8,192 | WAL write buffer size in bytes (equals `kPageSize`). |
| `kMaxLogRecordSize` | u32 | 256 | Maximum size of a single WAL log record in bytes. |

### Network

| Constant | Type | Value | Description |
|----------|------|-------|-------------|
| `kDefaultPort` | u16 | 5,433 | Default TCP listen port. |
| `kMaxConnections` | u32 | 64 | Default maximum concurrent connections. |

### Miscellaneous

| Constant | Type | Value | Description |
|----------|------|-------|-------------|
| `kCacheLineSize` | u32 | 64 | CPU cache line size in bytes; used for padding to avoid false sharing. |
| `kMaxPageChainHops` | u32 | 1,000,000 | Safety limit on page-chain traversal length to prevent infinite loops from corruption. |

---

## Loading Mechanism

Configuration is managed by the `DbConfigLoader` class
(`src/common/db_config.h`):

- **`DbConfigLoader::load_file(path, config, error)`** -- Reads a
  configuration file at the given path and populates a `DbConfig` struct. If
  the file does not exist (`ENOENT`), the call succeeds silently and all
  parameters keep their defaults. Returns `false` on parse errors, with a
  human-readable message written to the optional `error` output parameter.

- **`DbConfigLoader::describe(config)`** -- Returns a human-readable,
  newline-separated summary of all current settings in a `DbConfig` struct.
  Useful for logging the active configuration at startup.

### Config-file syntax

```
# This is a comment
shared_buffers   = 128MB
work_mem         = 64MB
wal_fsync        = on
statement_timeout = 10S
dirty_page_threshold = 80%
```

Unknown keys are silently ignored. Malformed values cause `load_file()` to
return `false` with the offending line number in the error message.
