#include "database/database.h"
#include "common/tuple_key.h"
#include "index/index_key.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <cstdlib>
#include <unistd.h>
#include <chrono>

namespace minidb {

// Forward decl — implementation lives further down so the constructor can
// honour MINIDB_FAULT=skip_index_rebuild for B5/D4 tests.
static bool fault_active(const char* name);

static bool btree_supports_type(TypeId type) {
    return type == TypeId::kBool || type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble ||
           type == TypeId::kVarchar || type == TypeId::kNull;
}

static IndexKey index_key_from_tuple(const IndexEntry& index, const Tuple& tuple) {
    Vector<Value> values;
    for (u32 i = 0; i < index.key_columns.size(); i++) {
        values.push_back(tuple.get_value(index.key_columns[i]));
    }
    return IndexKey::from_values(values);
}

static u32 pool_frames_from_config(const DbConfig& config) {
    u64 frames = config.shared_buffers_bytes / kPageSize;
    if (frames == 0) frames = 1;
    if (frames > kMaxPoolFrames) frames = kMaxPoolFrames;
    return static_cast<u32>(frames);
}

Database::Database(const String& db_dir)
    : Database(db_dir, DbConfig()) {}

Database::Database(const String& db_dir, const DbConfig& config)
    : db_dir_(db_dir), config_(config), txn_manager_(this), ops_since_gc_(0),
      background_running_(false) {
    mkdir(db_dir.c_str(), 0755);
    resources_ = UniquePtr<ResourceManager>(new ResourceManager(config_));
    disk_mgr_ = UniquePtr<DiskManager>(new DiskManager(db_dir, config_.doublewrite,
                                                       config_.page_checksum,
                                                       config_.fd_cache_limit));
    if (config_.storage_mode == "remote") {
        if (!config_.page_server_host.empty()) {
            page_store_ = UniquePtr<PageStore>(new RemotePageStoreClient(
                config_.page_server_host, config_.page_server_port,
                config_.storage_read_only, config_.storage_read_lsn,
                config_.remote_page_batch_size,
                config_.remote_connect_timeout_ms,
                config_.remote_io_timeout_ms,
                config_.remote_retry_count,
                config_.remote_max_connections));
        } else {
            String page_server_dir = config_.page_server_dir.empty()
                ? db_dir + "/page_server"
                : config_.page_server_dir;
            page_server_ = UniquePtr<PageServer>(new PageServer(page_server_dir,
                                                                config_.doublewrite,
                                                                config_.page_checksum,
                                                                config_.fd_cache_limit,
                                                                config_.page_server_replicas,
                                                                config_.page_server_cached_versions_per_page));
            page_store_ = UniquePtr<PageStore>(new RemotePageStore(page_server_.get(),
                                                                   config_.storage_read_only,
                                                                   config_.storage_read_lsn,
                                                                   config_.remote_page_batch_size));
        }
    } else {
        page_store_ = UniquePtr<PageStore>(new LocalPageStore(disk_mgr_.get()));
    }
    pool_ = UniquePtr<BufferPool>(new BufferPool(page_store_.get(), pool_frames_from_config(config_),
                                                 config_.buffer_pool_wait_timeout_ms,
                                                 config_.max_buffer_waiters,
                                                 config_.buffer_pool_partitions,
                                                 config_.remote_flush_batch_size));
    wal_ = UniquePtr<WalManager>(new WalManager(db_dir + "/wal",
                                                config_.wal_segment_size_bytes,
                                                config_.wal_fsync,
                                                config_.wal_group_commit,
                                                config_.wal_group_commit_delay_ms));
    pool_->set_wal_manager(wal_.get());
    // A1: hand the transaction manager its persistent CLOG. Lives next to
    // the WAL directory so backups copy it together. Loading it here means
    // any visibility check during recovery (e.g. via rebuild_all_indexes)
    // already sees historical commit/abort state.
    txn_manager_.set_status_log(
        UniquePtr<TxnStatusLog>(new TxnStatusLog(db_dir + "/wal")));
    load_catalog();
    bool has_control = load_control_file();
    if (wal_->recover(this)) {
        // WAL replay touched the heap; every index is now potentially stale.
        // Flip every entry to kInvalid before the rebuild starts, so that
        // any SQL that arrives between recover() returning and the rebuild
        // finishing (e.g. via background maintenance hitting the catalog
        // early) sees a guarded state machine rather than a half-built tree.
        struct InvalidateCtx { } _ctx;
        catalog_.for_each_index([](IndexEntry& e, void*) {
            e.state = IndexState::kInvalid;
        }, &_ctx);
        // Fault hook: tests use this to assert that the optimiser refuses
        // to use an index while it is in the kInvalid state. Production
        // code never sets MINIDB_FAULT.
        if (!fault_active("skip_index_rebuild")) {
            rebuild_all_indexes();
        }
        // Key: must flush after recovery to ensure replayed WAL records are persisted to data files
        flush();
    }
    if (!has_control && config_.startup_scan_txn_watermark) {
        advance_txn_id_watermark_from_storage();
    }
    if (config_.consistency_check_on_startup) {
        // C4 — opt-in fail-fast guard. After WAL replay and lazy index
        // rebuild the heap and indexes must agree on every live tuple.
        // If they do not, something corrupted the database state and we
        // refuse to open it rather than silently serving partial reads.
        String err;
        if (!check_table_index_consistency(&err)) {
            std::fprintf(stderr,
                         "minidb: startup consistency check failed: %s\n"
                         "        refusing to open '%s'. Set "
                         "consistency_check_on_startup = off to bypass.\n",
                         err.c_str(), db_dir_.c_str());
            std::exit(1);
        }
    }
    gc_ = UniquePtr<GarbageCollector>(new GarbageCollector(pool_.get(), &txn_manager_, &catalog_, this));

    // Collect statistics for query optimizer (on-demand via ANALYZE, not at startup)
    // collect_all_statistics();  // Re-enabled on first EXPLAIN if stats_valid==false

    // Initialize shared memory (4MB default size)
    shm_ = UniquePtr<SharedMemory>(SharedMemory::create("minidb_main", 4 * 1024 * 1024));
    start_background_maintenance();
}

Database::~Database() {
    stop_background_maintenance();
    save_catalog();
    for (auto it = heap_files_.begin(); it; it = heap_files_.next(it)) {
        if (it->value) it->value->flush_meta();
    }
    flush();
    save_control_file(true);
    fprintf(stderr, "[DB] destructor: flush complete, durable_lsn=%lu\n",
            static_cast<unsigned long>(wal_->durable_lsn()));
}

void Database::start_background_maintenance() {
    if (background_running_.load()) return;
    if (config_.checkpoint_timeout_ms == 0 && (!config_.gc_enabled || config_.gc_interval_ms == 0)) {
        return;
    }
    background_running_.store(true);
    background_thread_ = std::thread(&Database::background_maintenance_loop, this);
}

void Database::stop_background_maintenance() {
    background_running_.store(false);
    if (background_thread_.joinable()) {
        background_thread_.join();
    }
}

void Database::background_maintenance_loop() {
    using Clock = std::chrono::steady_clock;
    auto last_checkpoint = Clock::now();
    auto last_gc = Clock::now();
    while (background_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto now = Clock::now();
        if (config_.gc_enabled && config_.gc_interval_ms != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gc).count() >=
                static_cast<long long>(config_.gc_interval_ms)) {
            if (!txn_manager_.current()) {
                gc_->run_gc(config_.gc_max_pages_per_cycle);
            }
            last_gc = now;
        }
        bool checkpoint_by_time = config_.checkpoint_timeout_ms != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_checkpoint).count() >=
                static_cast<long long>(config_.checkpoint_timeout_ms);
        bool checkpoint_by_wal = config_.checkpoint_wal_size_bytes != 0 &&
            wal_ && wal_->bytes_since_checkpoint() >= config_.checkpoint_wal_size_bytes;
        if (checkpoint_by_time || checkpoint_by_wal) {
            checkpoint();
            last_checkpoint = now;
        }
    }
}

String Database::stats_summary() const {
    ResourceSnapshot rs = resources_ ? resources_->snapshot() : ResourceSnapshot();
    BufferPoolStats bs = pool_ ? pool_->stats() : BufferPoolStats();
    RemotePageStoreClientStats remote_client_stats;
    PageServerStats page_server_stats;
    const RemotePageStoreClient* remote_client =
        page_store_ ? dynamic_cast<const RemotePageStoreClient*>(page_store_.get()) : nullptr;
    if (remote_client) remote_client_stats = remote_client->stats();
    if (page_server_) page_server_stats = page_server_->stats();
    char buf[4096];
    std::snprintf(buf, sizeof(buf),
                  "active_connections=%u\n"
                  "active_queries=%u\n"
                  "active_write_queries=%u\n"
                  "waiting_queries=%u\n"
                  "active_transactions=%u\n"
                  "waiting_transactions=%u\n"
                  "memory_reserved=%lu\n"
                  "temp_bytes_used=%lu\n"
                  "rejected_connections=%lu\n"
                  "rejected_queries=%lu\n"
                  "admission_timeouts=%lu\n"
                  "transaction_timeouts=%lu\n"
                  "buffer_pool_frames=%u\n"
                  "buffer_pool_partitions=%u\n"
                  "buffer_hits=%lu\n"
                  "buffer_misses=%lu\n"
                  "buffer_waiters=%u\n"
                  "buffer_wait_timeouts=%lu\n"
                  "buffer_wait_rejections=%lu\n"
                  "dirty_pages=%u\n"
                  "storage_mode=%s\n"
                  "page_server_host=%s\n"
                  "page_server_port=%u\n"
                  "storage_read_only=%s\n"
                  "page_server_replicas=%u\n"
                  "remote_read_batches=%lu\n"
                  "remote_write_batches=%lu\n"
                  "remote_retries=%lu\n"
                  "remote_reconnects=%lu\n"
                  "remote_failures=%lu\n"
                  "pageserver_wal_image_bytes=%lu\n"
                  "pageserver_lazy_apply_hits=%lu\n"
                  "pageserver_future_page_fallbacks=%lu\n"
                  "pageserver_rejected_writes=%lu\n"
                  "wal_next_lsn=%lu\n"
                  "wal_durable_lsn=%lu\n"
                  "wal_bytes_since_checkpoint=%lu\n"
                  "wal_group_commit_batches=%lu\n"
                  "wal_buffer_flushes=%lu\n"
                  "wal_buffered_bytes=%lu\n"
                  "next_txn_id=%lu\n"
                  "gc_enabled=%s\n"
                  "gc_ops_threshold=%u\n"
                  "checkpoint_timeout_ms=%lu\n"
                  "checkpoint_wal_size=%lu\n",
                  rs.active_connections,
                  rs.active_queries,
                  rs.active_write_queries,
                  rs.waiting_queries,
                  rs.active_transactions,
                  rs.waiting_transactions,
                  static_cast<unsigned long>(rs.memory_reserved),
                  static_cast<unsigned long>(rs.temp_bytes_used),
                  static_cast<unsigned long>(rs.rejected_connections),
                  static_cast<unsigned long>(rs.rejected_queries),
                  static_cast<unsigned long>(rs.admission_timeouts),
                  static_cast<unsigned long>(rs.transaction_timeouts),
                  pool_ ? pool_->pool_size() : 0,
                  bs.partitions,
                  static_cast<unsigned long>(bs.hits),
                  static_cast<unsigned long>(bs.misses),
                  bs.waiters,
                  static_cast<unsigned long>(bs.wait_timeouts),
                  static_cast<unsigned long>(bs.wait_rejections),
                  bs.dirty_pages,
                  config_.storage_mode.c_str(),
                  config_.page_server_host.c_str(),
                  config_.page_server_port,
                  config_.storage_read_only ? "on" : "off",
                  page_server_ ? page_server_->replica_count() : 0,
                  static_cast<unsigned long>(remote_client_stats.read_batches),
                  static_cast<unsigned long>(remote_client_stats.write_batches),
                  static_cast<unsigned long>(remote_client_stats.retries),
                  static_cast<unsigned long>(remote_client_stats.reconnects),
                  static_cast<unsigned long>(remote_client_stats.failures),
                  static_cast<unsigned long>(page_server_stats.wal_image_bytes),
                  static_cast<unsigned long>(page_server_stats.lazy_apply_hits),
                  static_cast<unsigned long>(page_server_stats.future_page_fallbacks),
                  static_cast<unsigned long>(page_server_stats.rejected_writes),
                  static_cast<unsigned long>(wal_ ? wal_->next_lsn() : 0),
                  static_cast<unsigned long>(wal_ ? wal_->durable_lsn() : 0),
                  static_cast<unsigned long>(wal_ ? wal_->bytes_since_checkpoint() : 0),
                  static_cast<unsigned long>(wal_ ? wal_->group_commit_batches() : 0),
                  static_cast<unsigned long>(wal_ ? wal_->buffer_flushes() : 0),
                  static_cast<unsigned long>(wal_ ? wal_->buffered_bytes() : 0),
                  static_cast<unsigned long>(txn_manager_.next_txn_id()),
                  config_.gc_enabled ? "on" : "off",
                  config_.gc_ops_threshold,
                  static_cast<unsigned long>(config_.checkpoint_timeout_ms),
                  static_cast<unsigned long>(config_.checkpoint_wal_size_bytes));
    return String(buf);
}

bool Database::create_table(const String& name, const Schema& schema) {
    u32 tid = catalog_.create_table(name, schema);
    if (tid == 0) return false;
    heap_files_[tid] = UniquePtr<HeapFile>(new HeapFile(pool_.get(), tid));
    heap_files_[tid]->create();

    Vector<u32> auto_idx_ids;
    for (u32 i = 0; i < schema.column_count(); i++) {
        const Column& col = schema.get_column(i);
        if (!col.is_primary && !col.is_unique) continue;
        Vector<u32> key_cols;
        key_cols.push_back(i);
        String suffix = col.is_primary ? "_pkey" : "_key";
        u32 index_id = catalog_.create_index(name + "_" + col.name + suffix, tid,
                                             key_cols, true);
        if (index_id != 0) {
            rebuild_index(catalog_.get_index(index_id));
            auto_idx_ids.push_back(index_id);
        }
    }

    save_catalog();
        if (wal_) {
        u64 ddl_txn = txn_manager_.current() ? txn_manager_.current()->id() : 0;
        wal_->log_ddl(ddl_txn, DdlOp::kCreateTable, tid, 0, name);
    }

    // Record DDL undo if inside a transaction.
    Transaction* txn = txn_manager_.current();
    if (txn) {
        DdlUndoInfo info;
        info.table_name = name;
        info.auto_index_ids = static_cast<Vector<u32>&&>(auto_idx_ids);
        txn->record_ddl(UndoType::kDdlCreateTable, tid,
                        static_cast<DdlUndoInfo&&>(info));
    }
    return true;
}

bool Database::drop_table(const String& name) {
    TableEntry* table = catalog_.get_table(name);
    if (!table) return false;

    u32 table_id = table->table_id;

    // DDL lock: AccessExclusive blocks all concurrent access.
    if (txn_manager_.current()) {
        auto lock_res = lock_mgr_.lock_table(
            txn_manager_.current()->id(), table_id, LockMode::kAccessExclusive);
        if (!lock_res.ok()) return false;
    }
    Transaction* txn = txn_manager_.current();

    // Save info for DDL undo before modifying the catalog.
    DdlUndoInfo undo_info;
    if (txn) {
        undo_info.table_name = name;
        undo_info.saved_schema = table->schema;
        Vector<IndexEntry*> indexes = catalog_.get_indexes(table_id);
        for (u32 i = 0; i < indexes.size(); i++) {
            DdlSavedIndex si;
            si.index_id = indexes[i]->index_id;
            si.index_name = indexes[i]->index_name;
            si.table_id = indexes[i]->table_id;
            si.key_columns = indexes[i]->key_columns;
            si.is_unique = indexes[i]->is_unique;
            undo_info.saved_indexes.push_back(si);
            undo_info.deferred_deletes.push_back(
                String("indexes/") + String(si.index_id) + ".btree");
        }
        undo_info.deferred_deletes.push_back(
            String("tables/") + String(table_id) + ".heap");
    }

    Vector<IndexEntry*> indexes = catalog_.get_indexes(table_id);
    Vector<u32> index_ids;
    for (u32 i = 0; i < indexes.size(); i++) {
        index_ids.push_back(indexes[i]->index_id);
    }
    if (!catalog_.drop_table(name)) return false;

    for (u32 i = 0; i < index_ids.size(); i++) {
        index_trees_.erase(index_ids[i]);
        if (!txn && page_store_) {
            page_store_->delete_file(String("indexes/") + String(index_ids[i]) + ".btree");
        }
    }
    heap_files_.erase(table_id);
    if (!txn && page_store_) {
        page_store_->delete_file(String("tables/") + String(table_id) + ".heap");
    }
    save_catalog();
        if (wal_) {
        u64 ddl_txn = txn_manager_.current() ? txn_manager_.current()->id() : 0;
        wal_->log_ddl(ddl_txn, DdlOp::kDropTable, table_id, 0, name);
    }

    if (txn) {
        txn->record_ddl(UndoType::kDdlDropTable, table_id,
                        static_cast<DdlUndoInfo&&>(undo_info));
    }
    return true;
}

bool Database::drop_index(const String& name) {
    IndexEntry* index = catalog_.get_index(name);
    if (!index) return false;
    u32 index_id = index->index_id;
    u32 table_id = index->table_id;
    Transaction* txn = txn_manager_.current();

    // DDL lock: AccessExclusive on the parent table.
    if (txn) {
        auto lock_res = lock_mgr_.lock_table(txn->id(), table_id, LockMode::kAccessExclusive);
        if (!lock_res.ok()) return false;
    }

    // Save info for DDL undo before modifying the catalog.
    DdlUndoInfo undo_info;
    if (txn) {
        undo_info.single_index.index_id = index_id;
        undo_info.single_index.index_name = index->index_name;
        undo_info.single_index.table_id = table_id;
        undo_info.single_index.key_columns = index->key_columns;
        undo_info.single_index.is_unique = index->is_unique;
        undo_info.deferred_deletes.push_back(
            String("indexes/") + String(index_id) + ".btree");
    }

    if (!catalog_.drop_index(name)) return false;
    index_trees_.erase(index_id);
    if (!txn && page_store_) {
        page_store_->delete_file(String("indexes/") + String(index_id) + ".btree");
    }
    save_catalog();
        if (wal_) {
        u64 ddl_txn = txn_manager_.current() ? txn_manager_.current()->id() : 0;
        wal_->log_ddl(ddl_txn, DdlOp::kDropIndex, table_id, index_id, name);
    }

    if (txn) {
        txn->record_ddl(UndoType::kDdlDropIndex, table_id,
                        static_cast<DdlUndoInfo&&>(undo_info));
    }
    return true;
}

bool Database::create_index(const String& name, const String& table_name,
                            const Vector<String>& columns, bool unique) {
    TableEntry* table = catalog_.get_table(table_name);
    if (!table || catalog_.get_index(name)) return false;

    // DDL lock: Exclusive allows concurrent reads but blocks writes.
    if (txn_manager_.current()) {
        auto lock_res = lock_mgr_.lock_table(
            txn_manager_.current()->id(), table->table_id, LockMode::kExclusive);
        if (!lock_res.ok()) return false;
    }

    Vector<u32> key_cols;
    for (u32 i = 0; i < columns.size(); i++) {
        int idx = table->schema.get_column_index(columns[i]);
        if (idx < 0) return false;
        if (!btree_supports_type(table->schema.get_column(static_cast<u32>(idx)).type)) {
            return false;
        }
        key_cols.push_back(static_cast<u32>(idx));
    }

    IndexEntry probe;
    probe.key_columns = key_cols;
    {
        HeapFile* heap = get_heap_file(table->table_id);
        PageId first_page = heap->first_data_page_id();
        u32 file_id = file_id_from_page(first_page);
        u32 page_num = page_num_from_page(first_page);
        u32 pages = heap->meta().num_data_pages;
        for (u32 p = 0; p < pages; p++, page_num++) {
            PageId page_id = make_page_id(file_id, page_num);
            auto result = pool_->fetch_page(page_id, true);
            if (!result.ok()) continue;
            Page* page = result.value();
            u16 num_tuples = page->header()->num_tuples;
            for (u16 slot = 0; slot < num_tuples; slot++) {
                const LinePointer* lp = page->line_pointer(slot);
                if (!lp || !lp->is_valid()) continue;
                Tuple tuple = Tuple::deserialize_from_page(page->data() + lp->offset,
                                                           table->schema, lp->length);
                if (tuple.xmax() != kInvalidTxnId) continue;
                if (!index_key_from_tuple(probe, tuple).fits()) {
                    pool_->unpin_page(page_id);
                    return false;
                }
            }
            pool_->unpin_page(page_id);
        }
    }

    if (unique) {
        HashMap<String, bool> seen_keys;
        HeapFile* heap = get_heap_file(table->table_id);
        PageId first_page = heap->first_data_page_id();
        u32 file_id = file_id_from_page(first_page);
        u32 page_num = page_num_from_page(first_page);
        u32 pages = heap->meta().num_data_pages;
        for (u32 p = 0; p < pages; p++, page_num++) {
            PageId page_id = make_page_id(file_id, page_num);
            auto result = pool_->fetch_page(page_id, true);
            if (!result.ok()) continue;
            Page* page = result.value();
            u16 num_tuples = page->header()->num_tuples;

            for (u16 slot = 0; slot < num_tuples; slot++) {
                const LinePointer* lp = page->line_pointer(slot);
                if (!lp || !lp->is_valid()) continue;
                Tuple tuple = Tuple::deserialize_from_page(page->data() + lp->offset, table->schema, lp->length);
                if (tuple.xmax() != kInvalidTxnId) continue;
                String key;
                if (make_projected_tuple_key(tuple, key_cols, true, &key)) {
                    if (seen_keys.find(key)) {
                        pool_->unpin_page(page_id);
                        return false;
                    }
                    seen_keys.insert(key, true);
                }
            }
            pool_->unpin_page(page_id);
        }
    }

    u32 index_id = catalog_.create_index(name, table->table_id, key_cols, unique);
    if (index_id == 0) {
        return false;
    }
    IndexEntry* index = catalog_.get_index(name);
    rebuild_index(index);
    save_catalog();
        if (wal_) {
        u64 ddl_txn = txn_manager_.current() ? txn_manager_.current()->id() : 0;
        wal_->log_ddl(ddl_txn, DdlOp::kCreateIndex, table->table_id, index_id, name);
    }

    // Record DDL undo if inside a transaction.
    Transaction* txn = txn_manager_.current();
    if (txn) {
        DdlUndoInfo info;
        info.single_index.index_id = index_id;
        info.single_index.index_name = name;
        info.single_index.table_id = table->table_id;
        info.single_index.key_columns = key_cols;
        info.single_index.is_unique = unique;
        txn->record_ddl(UndoType::kDdlCreateIndex, table->table_id,
                        static_cast<DdlUndoInfo&&>(info));
    }
    return true;
}

static void set_alter_error(String* error, const char* msg) {
    if (error) *error = String(msg);
}

static bool table_has_live_rows(BufferPool* pool, HeapFile* heap, const Schema& schema) {
    if (!pool || !heap) return false;
    PageId first_page = heap->first_data_page_id();
    if (first_page == kNullPageId) return false;
    u32 file_id = file_id_from_page(first_page);
    u32 page_num = page_num_from_page(first_page);
    u32 pages = heap->meta().num_data_pages;
    for (u32 p = 0; p < pages; p++, page_num++) {
        PageId page_id = make_page_id(file_id, page_num);
        auto result = pool->fetch_page(page_id, true);
        if (!result.ok()) continue;
        Page* page = result.value();
        u16 num_tuples = page->header()->num_tuples;
        for (u16 slot = 0; slot < num_tuples; slot++) {
            const LinePointer* lp = page->line_pointer(slot);
            if (!lp || !lp->is_valid()) continue;
            Tuple tuple = Tuple::deserialize_from_page(page->data() + lp->offset,
                                                       schema, lp->length);
            if (tuple.xmax() == kInvalidTxnId) {
                pool->unpin_page(page_id);
                return true;
            }
        }
        pool->unpin_page(page_id);
    }
    return false;
}

bool Database::alter_table_add_column(const String& table_name, const Column& column,
                                      String* error) {
    TableEntry* table = catalog_.get_table(table_name);
    if (!table) {
        set_alter_error(error, "table not found");
        return false;
    }

    // DDL lock: AccessExclusive for ALTER TABLE.
    if (txn_manager_.current()) {
        auto lock_res = lock_mgr_.lock_table(
            txn_manager_.current()->id(), table->table_id, LockMode::kAccessExclusive);
        if (!lock_res.ok()) { set_alter_error(error, "could not acquire DDL lock"); return false; }
    }
    if (table->schema.get_column_index(column.name) >= 0) {
        set_alter_error(error, "column already exists");
        return false;
    }
    if (column.not_null && column.default_value.empty()) {
        HeapFile* heap = get_heap_file(table->table_id);
        if (table_has_live_rows(pool_.get(), heap, table->schema)) {
            set_alter_error(error, "cannot add NOT NULL column without DEFAULT to non-empty table");
            return false;
        }
    }

    u32 added_col_idx = table->schema.column_count();
    u32 tid = table->table_id;
    table->schema.add_column(column);
    save_catalog();
    checkpoint();
    // log_ddl runs AFTER checkpoint because checkpoint truncates the WAL;
    // writing the marker last guarantees it lives in the post-truncate
    // region for any future repair pass to find.
    if (wal_) {
        u64 ddl_txn = txn_manager_.current() ? txn_manager_.current()->id() : 0;
        wal_->log_ddl(ddl_txn, DdlOp::kAlterAddColumn, tid, added_col_idx,
                      table_name + "." + column.name);
    }

    // Record DDL undo if inside a transaction.
    Transaction* txn = txn_manager_.current();
    if (txn) {
        DdlUndoInfo info;
        info.table_name = table_name;
        info.column_position = added_col_idx;
        txn->record_ddl(UndoType::kDdlAlterAddColumn, tid,
                        static_cast<DdlUndoInfo&&>(info));
    }
    return true;
}

bool Database::alter_table_drop_column(const String& table_name, const String& column_name,
                                       String* error) {
    TableEntry* table = catalog_.get_table(table_name);
    if (!table) {
        set_alter_error(error, "table not found");
        return false;
    }

    // DDL lock: AccessExclusive for ALTER TABLE.
    if (txn_manager_.current()) {
        auto lock_res = lock_mgr_.lock_table(
            txn_manager_.current()->id(), table->table_id, LockMode::kAccessExclusive);
        if (!lock_res.ok()) { set_alter_error(error, "could not acquire DDL lock"); return false; }
    }
    // get_column_index already skips is_dropped columns, so re-dropping
    // a previously dropped column is rejected ("column not found").
    int col_idx_raw = table->schema.get_column_index(column_name);
    if (col_idx_raw < 0) {
        set_alter_error(error, "column not found");
        return false;
    }
    u32 col_idx = static_cast<u32>(col_idx_raw);

    // Reject dropping a column that participates in an index — user must
    // DROP INDEX first, same as PostgreSQL.
    Vector<IndexEntry*> indexes = catalog_.get_indexes(table->table_id);
    for (u32 i = 0; i < indexes.size(); i++) {
        IndexEntry* index = indexes[i];
        if (!index) continue;
        for (u32 k = 0; k < index->key_columns.size(); k++) {
            if (index->key_columns[k] == col_idx) {
                set_alter_error(error, "cannot drop indexed column");
                return false;
            }
        }
    }

    // PostgreSQL-style metadata-only DROP COLUMN: mark the column as
    // logically dropped. Physical tuple data stays unchanged — existing
    // rows still carry the old value, new rows store NULL in this slot.
    // No heap scan required, O(1) regardless of table size.
    // Column positions (used by indexes, tuple layout, etc.) do NOT shift.
    u32 tid = table->table_id;
    Column& col = const_cast<Column&>(table->schema.get_column(col_idx));
    col.is_dropped = true;
    save_catalog();
    checkpoint();
    if (wal_) {
        u64 ddl_txn = txn_manager_.current() ? txn_manager_.current()->id() : 0;
        wal_->log_ddl(ddl_txn, DdlOp::kAlterDropColumn, tid, col_idx,
                      table_name + "." + column_name);
    }

    // Record DDL undo if inside a transaction — rollback clears is_dropped.
    Transaction* txn = txn_manager_.current();
    if (txn) {
        DdlUndoInfo info;
        info.table_name = table_name;
        info.column_position = col_idx;
        txn->record_ddl(UndoType::kDdlAlterDropColumn, tid,
                        static_cast<DdlUndoInfo&&>(info));
    }
    return true;
}

bool Database::alter_table_rename_column(const String& table_name, const String& old_name,
                                         const String& new_name, String* error) {
    TableEntry* table = catalog_.get_table(table_name);
    if (!table) {
        set_alter_error(error, "table not found");
        return false;
    }

    // DDL lock: AccessExclusive for ALTER TABLE.
    if (txn_manager_.current()) {
        auto lock_res = lock_mgr_.lock_table(
            txn_manager_.current()->id(), table->table_id, LockMode::kAccessExclusive);
        if (!lock_res.ok()) { set_alter_error(error, "could not acquire DDL lock"); return false; }
    }
    int old_idx = table->schema.get_column_index(old_name);
    if (old_idx < 0) {
        set_alter_error(error, "column not found");
        return false;
    }
    if (table->schema.get_column_index(new_name) >= 0) {
        set_alter_error(error, "target column already exists");
        return false;
    }
    u32 col_pos = static_cast<u32>(old_idx);
    u32 tid = table->table_id;
    table->schema.rename_column(col_pos, new_name);
    save_catalog();
    checkpoint();
    if (wal_) {
        u64 ddl_txn = txn_manager_.current() ? txn_manager_.current()->id() : 0;
        wal_->log_ddl(ddl_txn, DdlOp::kAlterRenameColumn, tid, col_pos,
                      table_name + "." + old_name + "->" + new_name);
    }

    // Record DDL undo if inside a transaction.
    Transaction* txn = txn_manager_.current();
    if (txn) {
        DdlUndoInfo info;
        info.table_name = table_name;
        info.column_position = col_pos;
        info.rename_from = old_name;
        txn->record_ddl(UndoType::kDdlAlterRenameColumn, tid,
                        static_cast<DdlUndoInfo&&>(info));
    }
    return true;
}

// ============================================================
// DDL undo helpers — called by TransactionManager during rollback
// ============================================================

void Database::undo_create_table(u32 table_id, const DdlUndoInfo& info) {
    // Reverse a CREATE TABLE: drop auto-created indexes, then the table.
    for (u32 i = 0; i < info.auto_index_ids.size(); i++) {
        IndexEntry* idx = catalog_.get_index(info.auto_index_ids[i]);
        if (idx) {
            String path = String("indexes/") + String(idx->index_id) + ".btree";
            index_trees_.erase(idx->index_id);
            catalog_.drop_index(idx->index_name);
            if (page_store_) page_store_->delete_file(path);
        }
    }
    heap_files_.erase(table_id);
    if (page_store_) {
        page_store_->delete_file(String("tables/") + String(table_id) + ".heap");
    }
    catalog_.drop_table(info.table_name);
    save_catalog();
}

void Database::undo_drop_table(u32 table_id, const DdlUndoInfo& info) {
    // Reverse a DROP TABLE: restore catalog entries. Physical files were
    // kept on disk; HeapFile / BPlusTree wrappers will be recreated on
    // demand via get_heap_file() / get_index_tree().
    catalog_.restore_table(table_id, info.table_name, info.saved_schema);
    for (u32 i = 0; i < info.saved_indexes.size(); i++) {
        const DdlSavedIndex& si = info.saved_indexes[i];
        catalog_.restore_index(si.index_id, si.index_name, si.table_id,
                               si.key_columns, si.is_unique);
    }
    save_catalog();
}

void Database::undo_create_index(u32 /*table_id*/, const DdlUndoInfo& info) {
    u32 index_id = info.single_index.index_id;
    IndexEntry* idx = catalog_.get_index(index_id);
    if (idx) {
        index_trees_.erase(index_id);
        catalog_.drop_index(idx->index_name);
        if (page_store_) {
            page_store_->delete_file(String("indexes/") + String(index_id) + ".btree");
        }
    }
    save_catalog();
}

void Database::undo_drop_index(u32 /*table_id*/, const DdlUndoInfo& info) {
    const DdlSavedIndex& si = info.single_index;
    catalog_.restore_index(si.index_id, si.index_name, si.table_id,
                           si.key_columns, si.is_unique);
    IndexEntry* entry = catalog_.get_index(si.index_id);
    if (entry) rebuild_index(entry);
    save_catalog();
}

void Database::undo_alter_add_column(u32 table_id, const DdlUndoInfo& info) {
    TableEntry* table = catalog_.get_table(table_id);
    if (table && info.column_position < table->schema.column_count()) {
        table->schema.remove_column(info.column_position);
    }
    save_catalog();
    // No checkpoint() here — undo runs under the TransactionManager latch
    // and checkpoint() acquires the WAL latch, risking deadlock with the
    // background maintenance thread. The next regular checkpoint persists
    // the catalog change.
}

void Database::undo_alter_drop_column(u32 table_id, const DdlUndoInfo& info) {
    // Reverse a metadata-only DROP COLUMN: clear the is_dropped flag.
    TableEntry* table = catalog_.get_table(table_id);
    if (table && info.column_position < table->schema.column_count()) {
        Column& col = const_cast<Column&>(table->schema.get_column(info.column_position));
        col.is_dropped = false;
    }
    save_catalog();
}

void Database::undo_alter_rename_column(u32 table_id, const DdlUndoInfo& info) {
    TableEntry* table = catalog_.get_table(table_id);
    if (table && info.column_position < table->schema.column_count()) {
        table->schema.rename_column(info.column_position, info.rename_from);
    }
    save_catalog();
}

void Database::commit_ddl_deferred(const Vector<DdlUndoInfo>& ddl_infos) {
    for (u32 i = 0; i < ddl_infos.size(); i++) {
        const Vector<String>& files = ddl_infos[i].deferred_deletes;
        for (u32 j = 0; j < files.size(); j++) {
            if (page_store_) page_store_->delete_file(files[j]);
        }
    }
}

HeapFile* Database::get_heap_file(u32 table_id) {
    UniquePtr<HeapFile>* hf = heap_files_.find(table_id);
    if (hf) return hf->get();

    auto new_hf = UniquePtr<HeapFile>(new HeapFile(pool_.get(), table_id));
    HeapFile* ptr = new_hf.get();
    heap_files_[table_id] = static_cast<UniquePtr<HeapFile>&&>(new_hf);
    return ptr;
}

BPlusTree* Database::get_index_tree(u32 index_id) {
    UniquePtr<BPlusTree>* tree = index_trees_.find(index_id);
    if (tree) return tree->get();

    IndexEntry* entry = catalog_.get_index(index_id);
    if (!entry) return nullptr;

    auto new_tree = UniquePtr<BPlusTree>(new BPlusTree(index_id, pool_.get()));
    if (entry->root_page_id != kNullPageId && new_tree->open()) {
        BPlusTree* ptr = new_tree.get();
        index_trees_[index_id] = static_cast<UniquePtr<BPlusTree>&&>(new_tree);
        return ptr;
    }

    rebuild_index(entry);
    tree = index_trees_.find(index_id);
    return tree ? tree->get() : nullptr;
}

bool Database::validate_index_keys(u32 table_id, const Tuple& tuple) const {
    Vector<IndexEntry*> indexes = const_cast<Catalog&>(catalog_).get_indexes(table_id);
    for (u32 i = 0; i < indexes.size(); i++) {
        IndexEntry* index = indexes[i];
        if (!index) continue;
        IndexKey key = index_key_from_tuple(*index, tuple);
        if (!key.fits()) return false;
    }
    return true;
}

bool Database::read_tuple(u32 table_id, const Schema& schema, const RecordId& rid, Tuple* out) {
    HeapFile* heap = get_heap_file(table_id);
    if (!heap || !out) return false;
    auto result = pool_->fetch_page(rid.page_id, true);
    if (!result.ok()) return false;
    Page* page = result.value();
    const LinePointer* lp = page->line_pointer(rid.slot_idx);
    if (!lp || !lp->is_valid()) {
        pool_->unpin_page(rid.page_id);
        return false;
    }
    if (lp->offset >= kPageSize || static_cast<u32>(lp->offset) + lp->length > kPageSize) {
        pool_->unpin_page(rid.page_id);
        return false;
    }
    *out = Tuple::deserialize_from_page(page->data() + lp->offset, schema, lp->length);
    pool_->unpin_page(rid.page_id);
    return true;
}

// Fault-injection hook for ACID tests. Activated by setting the env var
// MINIDB_FAULT to a comma-separated list of named injection points. Has
// zero impact when unset, which is the production case.
static bool fault_active(const char* name) {
    const char* env = std::getenv("MINIDB_FAULT");
    if (!env || !*env) return false;
    size_t len = std::strlen(name);
    const char* p = env;
    while (*p) {
        const char* end = p;
        while (*end && *end != ',') end++;
        if (static_cast<size_t>(end - p) == len && std::strncmp(p, name, len) == 0) {
            return true;
        }
        if (!*end) break;
        p = end + 1;
    }
    return false;
}

bool Database::insert_index_entries(u32 table_id, const Tuple& tuple, const RecordId& rid) {
    Vector<IndexEntry*> indexes = catalog_.get_indexes(table_id);
    u64 txn_id = txn_manager_.current() ? txn_manager_.current()->id() : 1;
    // index_insert_silent mimics the pre-A2 bug for the C4 negative test:
    // skip the tree insert AND report success, leaving a heap row with no
    // matching index entry. Production code never sets this fault.
    const bool silent = fault_active("index_insert_silent");
    for (u32 i = 0; i < indexes.size(); i++) {
        IndexEntry* index = indexes[i];
        if (!index) continue;
        IndexKey key = index_key_from_tuple(*index, tuple);
        if (!key.fits()) continue;
        BPlusTree* tree = get_index_tree(index->index_id);
        if (!tree) continue;
        wal_->log_index_insert(txn_id, index->index_id, key, rid);
        if (silent) continue;
        if (fault_active("index_insert_fail") || !tree->insert(key, rid)) {
            // The heap tuple is in place but at least one index entry is
            // missing. Return false so the caller can surface an error;
            // the active transaction's undo records will then remove the
            // heap row plus any partial index entries via
            // delete_index_entries, which is idempotent.
            return false;
        }
    }
    return true;
}

void Database::delete_index_entries(u32 table_id, const Tuple& tuple, const RecordId& rid) {
    Vector<IndexEntry*> indexes = catalog_.get_indexes(table_id);
    u64 txn_id = txn_manager_.current() ? txn_manager_.current()->id() : 1;
    for (u32 i = 0; i < indexes.size(); i++) {
        IndexEntry* index = indexes[i];
        if (!index) continue;
        IndexKey key = index_key_from_tuple(*index, tuple);
        if (!key.fits()) continue;
        BPlusTree* tree = get_index_tree(index->index_id);
        if (tree) {
            wal_->log_index_delete(txn_id, index->index_id, key, rid);
            tree->remove(key, rid);
        }
    }
}

void Database::rebuild_indexes_for_table(u32 table_id) {
    Vector<IndexEntry*> indexes = catalog_.get_indexes(table_id);
    for (u32 i = 0; i < indexes.size(); i++) {
        rebuild_index(indexes[i]);
    }
}

void Database::rebuild_index(IndexEntry* index) {
    if (!index) return;
    TableEntry* table = catalog_.get_table(index->table_id);
    if (!table) return;

    // Transition state: kInvalid → kRebuilding → kValid. While the index
    // is being rebuilt the optimiser must refuse to use it; we keep the
    // state machine published in the catalog so concurrent planners (when
    // we add multi-statement concurrency) see a consistent view.
    index->state = IndexState::kRebuilding;

    auto tree = UniquePtr<BPlusTree>(new BPlusTree(index->index_id, pool_.get()));
    tree->create();
    BPlusTree* tree_ptr = tree.get();
    index_trees_[index->index_id] = static_cast<UniquePtr<BPlusTree>&&>(tree);
    index->root_page_id = tree_ptr->root_page_id();

    HeapFile* heap = get_heap_file(index->table_id);
    PageId first_page = heap->first_data_page_id();
    if (first_page == kNullPageId) {
        index->state = IndexState::kValid;   // empty table, trivially in sync
        return;
    }
    u32 file_id = file_id_from_page(first_page);
    u32 page_num = page_num_from_page(first_page);
    u32 pages = heap->meta().num_data_pages;
    for (u32 p = 0; p < pages; p++, page_num++) {
        PageId page_id = make_page_id(file_id, page_num);
        auto result = pool_->fetch_page(page_id, true);
        if (!result.ok()) continue;
        Page* page = result.value();
        u16 num_tuples = page->header()->num_tuples;

        for (u16 slot = 0; slot < num_tuples; slot++) {
            const LinePointer* lp = page->line_pointer(slot);
            if (!lp || !lp->is_valid()) continue;
            Tuple tuple = Tuple::deserialize_from_page(page->data() + lp->offset, table->schema, lp->length);
            if (tuple.xmax() != kInvalidTxnId) continue;
            IndexKey key = index_key_from_tuple(*index, tuple);
            if (key.fits()) {
                tree_ptr->insert(key, RecordId(page_id, slot));
            }
        }
        pool_->unpin_page(page_id);
    }
    // Atomic flip back to valid. A failure mid-rebuild would have returned
    // earlier and left the entry kRebuilding so the optimiser keeps refusing.
    index->state = IndexState::kValid;
}

void Database::rebuild_all_indexes() {
    struct IndexCtx {
        Database* db;
    } ctx{this};

    auto callback = [](IndexEntry& index, void* raw) {
        auto* ctx = static_cast<IndexCtx*>(raw);
        ctx->db->rebuild_index(&index);
    };
    catalog_.for_each_index(callback, &ctx);
}

bool Database::check_table_index_consistency(String* error) {
    struct CheckCtx {
        Database* db;
        String* error;
        bool ok;
    } ctx{this, error, true};

    auto callback = [](TableEntry& table, void* raw) {
        auto* ctx = static_cast<CheckCtx*>(raw);
        if (!ctx->ok) return;
        HeapFile* heap = ctx->db->get_heap_file(table.table_id);
        if (!heap) {
            ctx->ok = false;
            if (ctx->error && ctx->error->empty()) *ctx->error = String("missing heap file");
            return;
        }
        Vector<IndexEntry*> indexes = ctx->db->catalog_.get_indexes(table.table_id);
        if (indexes.empty()) return;

        PageId first_page = heap->first_data_page_id();
        if (first_page == kNullPageId) return;
        u32 file_id = file_id_from_page(first_page);
        u32 page_num = page_num_from_page(first_page);
        u32 pages = heap->meta().num_data_pages;

        for (u32 p = 0; ctx->ok && p < pages; p++, page_num++) {
            PageId page_id = make_page_id(file_id, page_num);
            auto page_result = ctx->db->pool_->fetch_page(page_id, true);
            if (!page_result.ok()) {
                ctx->ok = false;
                if (ctx->error && ctx->error->empty()) {
                    *ctx->error = String("failed to fetch heap page");
                }
                return;
            }
            Page* page = page_result.value();
            u16 num_tuples = page->header()->num_tuples;
            for (u16 slot = 0; ctx->ok && slot < num_tuples; slot++) {
                const LinePointer* lp = page->line_pointer(slot);
                if (!lp || !lp->is_valid()) continue;
                Tuple tuple = Tuple::deserialize_from_page(page->data() + lp->offset,
                                                           table.schema, lp->length);
                if (tuple.xmax() != kInvalidTxnId) continue;
                RecordId rid(page_id, slot);
                for (u32 i = 0; i < indexes.size(); i++) {
                    IndexEntry* index = indexes[i];
                    if (!index) continue;
                    BPlusTree* tree = ctx->db->get_index_tree(index->index_id);
                    if (!tree) {
                        ctx->ok = false;
                        if (ctx->error && ctx->error->empty()) {
                            *ctx->error = String("missing index tree");
                        }
                        break;
                    }
                    IndexKey key = index_key_from_tuple(*index, tuple);
                    if (key.is_null()) continue;
                    Vector<RecordId> found = tree->search(key);
                    bool matched = false;
                    for (u32 r = 0; r < found.size(); r++) {
                        if (found[r] == rid) {
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) {
                        ctx->ok = false;
                        if (ctx->error && ctx->error->empty()) {
                            *ctx->error = String("heap tuple missing from index");
                        }
                        break;
                    }
                }
            }
            ctx->db->pool_->unpin_page(page_id);
        }
    };

    catalog_.for_each_table(callback, &ctx);
    return ctx.ok;
}

void Database::save_catalog() {
    sync_catalog_metadata();
    catalog_.save(db_dir_ + "/catalog.mdbc");
}

void Database::load_catalog() {
    catalog_.load(db_dir_ + "/catalog.mdbc");
}

bool Database::load_control_file() {
    String path = db_dir_ + "/minidb.control";
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;

    char line[256];
    bool found_next = false;
    u64 next_txn = 0;
    u64 checkpoint_lsn = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "next_txn_id=", 12) == 0) {
            next_txn = static_cast<u64>(std::strtoull(line + 12, nullptr, 10));
            found_next = next_txn > 0;
        } else if (std::strncmp(line, "checkpoint_lsn=", 15) == 0) {
            checkpoint_lsn = static_cast<u64>(std::strtoull(line + 15, nullptr, 10));
        }
    }
    std::fclose(f);
    if (found_next) {
        txn_manager_.ensure_next_txn_id_at_least(next_txn);
    }
    // Restore the WAL high-water mark before recovery / first use. Clean
    // shutdown truncates the WAL file, so without this the next session's
    // LSNs would restart at 1 — smaller than the LSNs already stamped on
    // data pages — which violates monotonic-LSN and would deadlock the D2
    // checkpoint barrier (page_lsn > durable_lsn → flush_until under held
    // WAL latch).
    if (checkpoint_lsn != 0 && wal_) {
        wal_->ensure_next_lsn_at_least(checkpoint_lsn);
    }
    return found_next;
}

void Database::save_control_file(bool clean_shutdown) {
    String tmp = db_dir_ + "/minidb.control.tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "version=1\n");
    std::fprintf(f, "next_txn_id=%lu\n",
                 static_cast<unsigned long>(txn_manager_.next_txn_id()));
    std::fprintf(f, "checkpoint_lsn=%lu\n",
                 static_cast<unsigned long>(wal_->durable_lsn()));
    std::fprintf(f, "clean_shutdown=%s\n", clean_shutdown ? "true" : "false");
    std::fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    std::fclose(f);
    String final = db_dir_ + "/minidb.control";
    std::rename(tmp.c_str(), final.c_str());
}

void Database::advance_txn_id_watermark_from_storage() {
    struct Ctx {
        Database* db;
        u64 max_txn;
    };
    Ctx ctx = {this, 0};
    auto callback = [](TableEntry& table, void* raw) {
        auto* ctx = static_cast<Ctx*>(raw);
        HeapFile* heap = ctx->db->get_heap_file(table.table_id);
        if (!heap) return;
        PageId first_page = heap->first_data_page_id();
        if (first_page == kNullPageId) return;
        u32 file_id = file_id_from_page(first_page);
        u32 page_num = page_num_from_page(first_page);
        u32 pages = heap->meta().num_data_pages;
        for (u32 p = 0; p < pages; p++, page_num++) {
            PageId page_id = make_page_id(file_id, page_num);
            auto result = ctx->db->pool_->fetch_page(page_id, true);
            if (!result.ok()) continue;
            Page* page = result.value();
            u16 num_tuples = page->header()->num_tuples;
            for (u16 slot = 0; slot < num_tuples; slot++) {
                const LinePointer* lp = page->line_pointer(slot);
                if (!lp || !lp->is_valid()) continue;
                Tuple tuple = Tuple::deserialize_from_page(
                    page->data() + lp->offset, table.schema, lp->length);
                if (tuple.xmin() > ctx->max_txn) ctx->max_txn = tuple.xmin();
                if (tuple.xmax() > ctx->max_txn) ctx->max_txn = tuple.xmax();
            }
            ctx->db->pool_->unpin_page(page_id);
        }
    };
    catalog_.for_each_table(callback, &ctx);
    if (ctx.max_txn > 0) {
        txn_manager_.ensure_next_txn_id_at_least(ctx.max_txn + 1);
    }
}

void Database::flush() {
    wal_->flush();
    if (page_store_) page_store_->set_durable_lsn(wal_->durable_lsn());
    pool_->flush_all();
    if (page_store_) page_store_->flush();
    // Sync table row counts for optimizer cost estimation
    for (auto it = heap_files_.begin(); it; it = heap_files_.next(it)) {
        TableEntry* table = catalog_.get_table(it->key);
        if (!table || !it->value) continue;
        table->num_tuples = it->value->meta().num_tuples;
        table->stat_num_tuples = it->value->meta().num_tuples;
        table->stat_num_pages = it->value->meta().num_data_pages;
    }
}

void Database::flush_pages_for_checkpoint_trampoline(void* ctx) {
    static_cast<Database*>(ctx)->flush_pages_for_checkpoint();
}

void Database::flush_pages_for_checkpoint() {
    // Called by WalManager::checkpoint() with the WAL latch held. At this
    // point durable_lsn_ == last_written_lsn_, so every dirty page has
    // page_lsn <= durable_lsn_ and BufferPool::flush_frame_wal_first will
    // take its fast path instead of re-entering the WAL latch.
    if (page_store_) page_store_->set_durable_lsn(wal_->durable_lsn());
    pool_->flush_all();
    if (page_store_) page_store_->flush();
    for (auto it = heap_files_.begin(); it; it = heap_files_.next(it)) {
        TableEntry* table = catalog_.get_table(it->key);
        if (!table || !it->value) continue;
        table->num_tuples = it->value->meta().num_tuples;
        table->stat_num_tuples = it->value->meta().num_tuples;
        table->stat_num_pages = it->value->meta().num_data_pages;
    }
}

void Database::checkpoint() {
    for (auto it = heap_files_.begin(); it; it = heap_files_.next(it)) {
        if (it->value) it->value->flush_meta();
    }
    wal_->checkpoint(&Database::flush_pages_for_checkpoint_trampoline, this);
    save_control_file(false);
}

void Database::maybe_gc() {
    // Bulk load phase typically runs in explicit transaction (BEGIN...COMMIT),
    // we skip GC here to avoid high-frequency full table scans slowing down import throughput.
    if (txn_manager_.current()) {
        return;
    }

    if (!config_.gc_enabled) {
        return;
    }

    u32 threshold = config_.gc_ops_threshold == 0 ? 1 : config_.gc_ops_threshold;
    ops_since_gc_++;
    if (ops_since_gc_ >= threshold) {
        ops_since_gc_ = 0;
        gc_->run_gc(config_.gc_max_pages_per_cycle);
        // No checkpoint() here — GC changes (slot cleanup, pruning) are
        // idempotent and will be re-derived on recovery. Dirty pages are
        // persisted by the next background checkpoint (default 60s interval).
        // Calling checkpoint() on every GC cycle was the primary UPDATE
        // performance bottleneck: it flushed ALL dirty pages, truncated the
        // WAL, and wrote the control file — costing ~5-10ms per invocation.
    }
}

void Database::vacuum() {
    if (gc_) {
        gc_->run_vacuum();
        checkpoint();
    }
}

void Database::sync_catalog_metadata() {
    for (auto it = heap_files_.begin(); it; it = heap_files_.next(it)) {
        if (!it->value) continue;
        it->value->flush_meta();
        TableEntry* table = catalog_.get_table(it->key);
        if (!table) continue;
        const HeapMeta& meta = it->value->meta();
        table->first_page_id = it->value->first_data_page_id();
        table->num_pages = meta.num_data_pages;
        table->num_tuples = meta.num_tuples;
        table->stat_num_pages = meta.num_data_pages;
        table->stat_num_tuples = meta.num_tuples;
    }

    for (auto it = index_trees_.begin(); it; it = index_trees_.next(it)) {
        if (!it->value) continue;
        IndexEntry* index = catalog_.get_index(it->key);
        if (index) index->root_page_id = it->value->root_page_id();
    }
}

// W18: Collect column-level statistics for the specified table
void Database::collect_statistics(u32 table_id) {
    TableEntry* table = catalog_.get_table(table_id);
    if (!table) return;
    HeapFile* heap = get_heap_file(table_id);
    if (!heap) return;

    u32 num_cols = table->schema.column_count();
    table->col_stats.resize(num_cols);

    constexpr u32 kExactNdvLimitPerColumn = 100000;
    Vector<bool> ndv_capped;
    ndv_capped.resize(num_cols);

    // Initialize per-column stats
    HashMap<String, bool> seen_values;
    for (u32 c = 0; c < num_cols; c++) {
        table->col_stats[c] = ColumnStats();
        ndv_capped[c] = false;
    }

    PageId first_page = heap->first_data_page_id();
    if (first_page == kNullPageId) {
        table->stats_valid = true;
        return;
    }
    u32 file_id = file_id_from_page(first_page);
    u32 page_num = page_num_from_page(first_page);
    u32 pages = heap->meta().num_data_pages;

    // Scan all pages
    for (u32 p = 0; p < pages; p++, page_num++) {
        PageId page_id = make_page_id(file_id, page_num);
        auto result = pool_->fetch_page(page_id, true);
        if (!result.ok()) continue;

        Page* page = result.value();
        u16 num_tuples = page->header()->num_tuples;

        for (u16 slot = 0; slot < num_tuples; slot++) {
            const LinePointer* lp = page->line_pointer(slot);
            if (!lp || !lp->is_valid()) continue;

            Tuple tuple = Tuple::deserialize_from_page(
                page->data() + lp->offset, table->schema, lp->length);

            for (u32 c = 0; c < num_cols; c++) {
                Value val = tuple.get_value(c);
                ColumnStats& stats = table->col_stats[c];

                if (val.is_null()) {
                    stats.null_count++;
                    continue;
                }

                // NDV: exact until the per-column cap, using the same typed value encoding
                // as hash-based executor paths so non-int values do not collapse together.
                if (!ndv_capped[c]) {
                    String key(static_cast<u64>(c));
                    key += ':';
                    key += encode_value_key(val);
                    if (!seen_values.find(key)) {
                        seen_values.insert(key, true);
                        stats.ndv++;
                        if (stats.ndv >= kExactNdvLimitPerColumn) {
                            ndv_capped[c] = true;
                        }
                    }
                }

                // Min/Max (numeric types only)
                if (val.type_id() == TypeId::kInt32 || val.type_id() == TypeId::kInt64 ||
                    val.type_id() == TypeId::kFloat || val.type_id() == TypeId::kDouble) {
                    if (!stats.has_min_max) {
                        stats.min_val = val;
                        stats.max_val = val;
                        stats.has_min_max = true;
                    } else {
                        if (val < stats.min_val) stats.min_val = val;
                        if (val > stats.max_val) stats.max_val = val;
                    }
                }
            }
        }

        pool_->unpin_page(page_id);
    }

    table->stats_valid = true;
    table->stat_num_tuples = heap->meta().num_tuples;
    table->stat_num_pages = heap->meta().num_data_pages;
}

void Database::collect_all_statistics() {
    struct Ctx { Database* db; };
    Ctx ctx{this};
    auto cb = [](TableEntry& te, void* c) {
        auto* ctx = static_cast<Ctx*>(c);
        ctx->db->collect_statistics(te.table_id);
    };
    catalog_.for_each_table(cb, &ctx);
}

} // namespace minidb
