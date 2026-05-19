# WAL and Recovery Protocol

MiniDB uses write-ahead logging for heap/index changes and enforces the WAL-first rule before dirty pages reach the page store.

## Ordering Rules

1. Each logical change receives an LSN from `WalManager::write_record`.
2. The modified page records the latest page LSN in its page header.
3. Before `BufferPool` evicts, flushes, or batches a dirty page, it calls `WalManager::flush_until(page_lsn)`.
4. The page store durable LSN is updated only after WAL flush succeeds.
5. If WAL cannot be flushed to at least the page LSN, the dirty page write is skipped and eviction fails with an I/O error.

This means a persistent page never contains an LSN newer than durable WAL.

## Commit Rules

Commit records are flushed through `WalManager::flush_commit`. Group commit may delay the fsync by the configured window, but commit durability still depends on WAL becoming durable, not on data page flush.

## Replay Rules

Recovery scans `wal.log`, records committed and aborted transaction state, replays committed changes, and rebuilds index state when needed. Large or malformed records are bounded during replay to avoid reading arbitrary lengths from a corrupt WAL file.

## Current Boundaries

- WAL segments are represented by the active `wal.log` file plus configured truncation/checkpoint behavior.
- Record payloads are bounded during replay.
- Index WAL keys are dynamically buffered and support keys larger than the WAL page buffer as long as the per-record key length fits the on-disk `u16` key length field.
