# storage

This directory contains MiniDB's page storage layer. The current implementation supports both local single-node storage and an experimental shared-storage path through PageServer.

## Main Components

| Component | Files | Role |
| --- | --- | --- |
| Page layout | `page.h`, `page.cpp` | 8KB page buffer, page header, line pointers, tuple insertion/reclamation |
| Disk IO | `disk_manager.h`, `disk_manager.cpp` | Local page file read/write, double-write buffer, page checksum, file descriptor cache |
| Buffer pool | `buffer_pool.h`, `buffer_pool.cpp` | Partitioned page table/LRU, pin/unpin, dirty tracking, WAL-before-page flush, batched dirty-page writes |
| Heap storage | `heap_file.h`, `heap_file.cpp` | Table heap metadata and data-page allocation |
| PageStore abstraction | `page_store.h`, `page_store.cpp` | Common local/remote page API used by BufferPool |
| PageServer | `page_server.h`, `page_server.cpp` | Remote page service core, persistent metadata, remote WAL page images, LogIndex rebuild |
| PageServer TCP | `page_server_tcp.h`, `page_server_tcp.cpp`, `page_server_rpc.h` | Binary TCP protocol for standalone PageServer |
| Remote client | `remote_page_store_client.h`, `remote_page_store_client.cpp` | Compute-side TCP PageStore client with batch IO, timeouts, retries, and connection reuse |

## Page Layout

MiniDB uses fixed 8KB pages.

```text
PageHeader
LinePointer[]
free space
tuple data growing from page end
```

`PageId` is encoded as:

```text
PageId = (file_id << 32) | page_num
```

The `lsn` stored in `PageHeader` is used by WAL-before-page flushing and by PageServer future-page/snapshot checks.

## Local Storage Path

```text
BufferPool -> LocalPageStore -> DiskManager -> tables/indexes files
```

Implemented local protections:

- Double-write buffer for page writes.
- Page checksum validation.
- LRU file descriptor cache.
- BufferPool partitions for page-table/LRU lock reduction.
- Batched dirty-page writes from `flush_all()`.

## Remote Storage Path

In remote mode, BufferPool talks to a `PageStore` implementation instead of directly using `DiskManager`.

In-process test mode:

```text
BufferPool -> RemotePageStore -> PageServer
```

Standalone TCP mode:

```text
BufferPool -> RemotePageStoreClient -> TCP -> minidb_pageserver -> PageServer
```

Implemented remote behavior:

- Batch page read/write RPCs.
- Connect timeout, IO timeout, retry count, and connection reuse.
- PageServer connection admission limit.
- Durable-LSN check before accepting page writes.
- Persistent `remote_wal_images.bin` page-image log.
- Persistent `page_server.meta`.
- LogIndex rebuilt on PageServer startup by scanning `remote_wal_images.bin`.
- Read-only snapshot reads using `storage_read_lsn`.
- Future-page fallback using persisted LogIndex/WAL image data.
- Optional synchronous local replica directories via `page_server_replicas`.

Current remote limitations:

- PageServer replicas are directories, not independent follower processes.
- No Raft/quorum replication.
- No automatic failover.
- No distributed lock service.
- No multi-writer distributed transaction protocol.
- Remote redo stores full page images rather than compact physical delta records.

## PageServer Files

Standalone PageServer directory:

```text
pageserver-data/
├── page_server.meta
├── remote_wal_images.bin
├── doublewrite.bin
├── tables/
├── indexes/
└── replica_1/
```

`remote_wal_images.bin` stores page images with page id, LSN, page size, and checksum. On restart, PageServer scans it to rebuild:

```text
page_id -> [(lsn, wal_image_offset)]
page_id -> latest_lsn
```

## ACID-relevant invariants

- **WAL-first** — `BufferPool::flush_frame_wal_first()` runs `wal_->flush_until(page_lsn)`
  before any dirty page leaves the pool.
- **Torn-page protection** — `DiskManager` writes a doublewrite copy
  with magic + checksum before overwriting the main page, and
  `recover_double_write()` restores from it on startup (D4).
- **Checkpoint barrier** — `WalManager::checkpoint(cb, ctx)` runs the
  page flush callback inside the WAL latch so writers can't sneak
  records into the about-to-be-truncated WAL window (D2).

## Tests

Relevant tests:

```bash
./build/tests/page_store_remote_test
./build/tests/wal_buffer_pool_test
bash tests/storage/remote_page_store.sh ./build/minidb
ctest --test-dir build -R 'page_store_remote_test|remote_page_store|torn_page|checkpoint_barrier' --output-on-failure
```

These tests cover local PageStore compatibility, TCP remote reads/writes, batch IO, PageServer restart recovery, persisted metadata/WAL image files, RO snapshot reads, future-page handling, replica directory writes, WAL-first eviction ordering, torn-page protection (`tests/acid/durability/torn_page.py`), and the checkpoint barrier (`tests/acid/durability/checkpoint_barrier.py`).
