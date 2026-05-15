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

class GarbageCollector {
public:
    GarbageCollector(BufferPool* pool, TransactionManager* txn_mgr, Catalog* catalog);

    // Execute one incremental GC (process max_pages pages)
    void run_gc(u32 max_pages = 64);

private:
    bool is_garbage(const Tuple& t, u64 oldest_active_txn);

    BufferPool* pool_;
    TransactionManager* txn_mgr_;
    Catalog* catalog_;

    // Incremental GC state: per-table last processed page index
    HashMap<u32, u32> last_gc_page_;
};

} // namespace minidb
