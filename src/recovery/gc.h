/**
 * @file gc.h
 * @brief MVCC Garbage Collection — Incremental, supports Visibility Map to skip clean pages
 */
#pragma once

#include "common/defs.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "record/tuple.h"
#include "container/hash_map.h"

namespace minidb {

class TransactionManager;
class Catalog;
class Database;

class GarbageCollector {
public:
    GarbageCollector(BufferPool* pool, TransactionManager* txn_mgr, Catalog* catalog,
                     Database* db = nullptr);

    // Execute one incremental GC (process max_pages pages)
    void run_gc(u32 max_pages = 64);

private:
    bool is_garbage(const Tuple& t, u64 oldest_active_txn);

    BufferPool* pool_;
    TransactionManager* txn_mgr_;
    Catalog* catalog_;
    // Lets GC reach the live B+ tree handles via Database::delete_index_entries.
    // Needed because, under lazy index cleanup, GC is the only path that
    // removes an index entry for a no-longer-visible tuple — DELETE itself
    // intentionally leaves entries behind so older snapshots can still find
    // and visibility-check the row.
    Database* db_;

    // Incremental GC state: per-table last processed page index
    HashMap<u32, u32> last_gc_page_;
};

} // namespace minidb
