/**
 * @file database.h
 * @brief Database — Unified management of DiskManager, BufferPool, Catalog, HeapFile, LockManager, SharedMemory
 */
#pragma once

#include "common/defs.h"
#include "common/db_config.h"
#include "common/resource_manager.h"
#include "storage/disk_manager.h"
#include "storage/buffer_pool.h"
#include "storage/page_store.h"
#include "storage/page_server.h"
#include "storage/remote_page_store_client.h"
#include "storage/heap_file.h"
#include "storage/shared_memory.h"
#include "catalog/catalog.h"
#include "transaction/transaction.h"
#include "recovery/wal.h"
#include "recovery/gc.h"
#include "concurrency/lock_manager.h"
#include "index/btree.h"
#include "container/string.h"
#include "container/unique_ptr.h"
#include "container/hash_map.h"
#include <atomic>
#include <thread>

namespace minidb {

class Database {
public:
    explicit Database(const String& db_dir = "minidb_data");
    Database(const String& db_dir, const DbConfig& config);
    ~Database();

    bool create_table(const String& name, const Schema& schema);
    bool drop_table(const String& name);
    bool drop_index(const String& name);
    bool create_index(const String& name, const String& table_name,
                      const Vector<String>& columns, bool unique);
    HeapFile* get_heap_file(u32 table_id);
    BPlusTree* get_index_tree(u32 index_id);
    void insert_index_entries(u32 table_id, const Tuple& tuple, const RecordId& rid);
    void delete_index_entries(u32 table_id, const Tuple& tuple, const RecordId& rid);
    void rebuild_indexes_for_table(u32 table_id);
    bool read_tuple(u32 table_id, const Schema& schema, const RecordId& rid, Tuple* out);
    TableEntry* get_table(const String& name) { return catalog_.get_table(name); }
    Catalog& catalog() { return catalog_; }
    BufferPool& pool() { return *pool_; }
    void save_catalog();
    void load_catalog();
    void flush();
    void checkpoint();
    TransactionManager& txn_manager() { return txn_manager_; }
    const TransactionManager& txn_manager() const { return txn_manager_; }
    WalManager& wal() { return *wal_; }
    const DbConfig& config() const { return config_; }
    ResourceManager& resources() { return *resources_; }
    String stats_summary() const;
    LockManager& lock_manager() { return lock_mgr_; }
    SharedMemory* shared_memory() { return shm_.get(); }
    void maybe_gc();
    void collect_statistics(u32 table_id);  // W18: Collect column-level statistics
    void collect_all_statistics();           // W18: Collect statistics for all tables
    bool check_table_index_consistency(String* error = nullptr);

private:
    void background_maintenance_loop();
    void start_background_maintenance();
    void stop_background_maintenance();
    void rebuild_index(IndexEntry* index);
    void rebuild_all_indexes();
    void sync_catalog_metadata();
    void advance_txn_id_watermark_from_storage();
    bool load_control_file();
    void save_control_file(bool clean_shutdown);

    String db_dir_;
    DbConfig config_;
    UniquePtr<DiskManager> disk_mgr_;
    UniquePtr<PageStore> page_store_;
    UniquePtr<PageServer> page_server_;
    UniquePtr<ResourceManager> resources_;
    UniquePtr<BufferPool> pool_;
    Catalog catalog_;
    TransactionManager txn_manager_;
    LockManager lock_mgr_;
    UniquePtr<SharedMemory> shm_;
    UniquePtr<WalManager> wal_;
    UniquePtr<GarbageCollector> gc_;
    u32 ops_since_gc_;
    std::atomic<bool> background_running_;
    std::thread background_thread_;
    HashMap<u32, UniquePtr<HeapFile>> heap_files_;
    HashMap<u32, UniquePtr<BPlusTree>> index_trees_;
};

} // namespace minidb
