# common

Cross-cutting headers: integer aliases, mutex primitives, configuration
loader, resource admission control.

## Key types

| Type | File | Role |
| --- | --- | --- |
| Type aliases (`u8`/`u16`/.../`PageId`/`SlotIdx`/`LSN`/`TxnId`) | `defs.h` | Standard sized integer aliases used everywhere |
| Constants (`kPageSize`, `kMaxConnections`, ...) | `config.h`, `defs.h` | Compile-time defaults |
| `Mutex`, `CondVar`, `RwLock`, `LockGuard`, `WriteGuard`, `ReadGuard` | `mutex.h`, `mutex.cpp` | Non-recursive locking primitives used by storage / transaction / WAL |
| `DbConfig`, `DbConfigLoader` | `db_config.h`, `db_config.cpp` | Runtime configuration struct + INI-style key=value loader |
| `ResourceManager` | `resource_manager.h`, `resource_manager.cpp` | Admission control for connections / queries / writers / memory / temp files / active transactions |
| `NonCopyable` | `noncopyable.h` | Boilerplate base class |
| `tuple_key` helpers | `tuple_key.h` | Composite-key construction utilities used by executors |
| `atomic.h` | `atomic.h` | Aliases over `std::atomic<...>` for sized integers |
| `status.h` | `status.h` | `Status` enum + `Result<T>` used by storage / page-store APIs |

## Configuration

`DbConfig` is a plain struct holding every tuneable. Defaults match the
README; `DbConfigLoader::load_file()` parses `key=value` lines and
supports units (`B`, `KB`, `MB`, `GB`, `MS`, `S`, `MIN`). Loader
recognises ACID-relevant flags:

- `wal_fsync`, `wal_group_commit`, `wal_group_commit_delay`
- `checkpoint_timeout`, `checkpoint_wal_size`
- `doublewrite`, `page_checksum`
- `consistency_check_on_startup`
- `startup_scan_txn_watermark`

Run `./build/minidb --show-config` to dump the effective configuration.

## ResourceManager

Bounds:

- `max_connections`
- `max_active_queries`, `max_active_write_queries`
- `max_active_transactions`
- `query_memory_limit`, `work_mem`, `temp_file_limit`

Acquisition functions return false when the bound is exceeded; the
caller surfaces an admission error and aborts the operation.
