#include "database/database.h"
#include "common/tuple_key.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <cstdlib>
#include <unistd.h>
#include <chrono>

namespace minidb {

static bool btree_supports_type(TypeId type) {
    return type == TypeId::kBool || type == TypeId::kInt32 || type == TypeId::kInt64 ||
           type == TypeId::kFloat || type == TypeId::kDouble;
}

static Value index_key_from_tuple(const IndexEntry& index, const Tuple& tuple) {
    if (index.key_columns.size() == 1) {
        const Column& col = tuple.schema().get_column(index.key_columns[0]);
        if (btree_supports_type(col.type)) {
            return tuple.get_value(index.key_columns[0]);
        }
    }

    String key;
    if (!make_projected_tuple_key(tuple, index.key_columns, true, &key)) {
        return Value();
    }
    return Value(static_cast<i64>(Hash<String>()(key)));
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
    pool_ = UniquePtr<BufferPool>(new BufferPool(disk_mgr_.get(), pool_frames_from_config(config_),
                                                 config_.buffer_pool_wait_timeout_ms,
                                                 config_.max_buffer_waiters,
                                                 config_.buffer_pool_partitions));
    wal_ = UniquePtr<WalManager>(new WalManager(db_dir + "/wal",
                                                config_.wal_segment_size_bytes,
                                                config_.wal_fsync,
                                                config_.wal_group_commit,
                                                config_.wal_group_commit_delay_ms));
    pool_->set_wal_manager(wal_.get());
    load_catalog();
    bool has_control = load_control_file();
    if (wal_->recover(this)) {
        rebuild_all_indexes();
        // Key: must flush after recovery to ensure replayed WAL records are persisted to data files
        flush();
    }
    if (!has_control && config_.startup_scan_txn_watermark) {
        advance_txn_id_watermark_from_storage();
    }
    gc_ = UniquePtr<GarbageCollector>(new GarbageCollector(pool_.get(), &txn_manager_, &catalog_));

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
    char buf[2048];
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
        }
    }

    save_catalog();
    return true;
}

bool Database::drop_table(const String& name) {
    TableEntry* table = catalog_.get_table(name);
    if (!table) return false;

    u32 table_id = table->table_id;
    Vector<IndexEntry*> indexes = catalog_.get_indexes(table_id);
    Vector<u32> index_ids;
    for (u32 i = 0; i < indexes.size(); i++) {
        index_ids.push_back(indexes[i]->index_id);
    }
    if (!catalog_.drop_table(name)) return false;

    for (u32 i = 0; i < index_ids.size(); i++) {
        index_trees_.erase(index_ids[i]);
        disk_mgr_->delete_file(String("indexes/") + String(index_ids[i]) + ".btree");
    }
    heap_files_.erase(table_id);
    disk_mgr_->delete_file(String("tables/") + String(table_id) + ".heap");
    save_catalog();
    return true;
}

bool Database::drop_index(const String& name) {
    IndexEntry* index = catalog_.get_index(name);
    if (!index) return false;
    u32 index_id = index->index_id;
    if (!catalog_.drop_index(name)) return false;
    index_trees_.erase(index_id);
    disk_mgr_->delete_file(String("indexes/") + String(index_id) + ".btree");
    save_catalog();
    return true;
}

bool Database::create_index(const String& name, const String& table_name,
                            const Vector<String>& columns, bool unique) {
    TableEntry* table = catalog_.get_table(table_name);
    if (!table || catalog_.get_index(name)) return false;

    Vector<u32> key_cols;
    for (u32 i = 0; i < columns.size(); i++) {
        int idx = table->schema.get_column_index(columns[i]);
        if (idx < 0) return false;
        key_cols.push_back(static_cast<u32>(idx));
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
    return true;
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

void Database::insert_index_entries(u32 table_id, const Tuple& tuple, const RecordId& rid) {
    Vector<IndexEntry*> indexes = catalog_.get_indexes(table_id);
    u64 txn_id = txn_manager_.current() ? txn_manager_.current()->id() : 1;
    for (u32 i = 0; i < indexes.size(); i++) {
        IndexEntry* index = indexes[i];
        if (!index) continue;
        Value key = index_key_from_tuple(*index, tuple);
        if (key.is_null()) continue;
        BPlusTree* tree = get_index_tree(index->index_id);
        if (tree) {
            wal_->log_index_insert(txn_id, index->index_id, key, rid);
            tree->insert(key, rid);
        }
    }
}

void Database::delete_index_entries(u32 table_id, const Tuple& tuple, const RecordId& rid) {
    Vector<IndexEntry*> indexes = catalog_.get_indexes(table_id);
    u64 txn_id = txn_manager_.current() ? txn_manager_.current()->id() : 1;
    for (u32 i = 0; i < indexes.size(); i++) {
        IndexEntry* index = indexes[i];
        if (!index) continue;
        Value key = index_key_from_tuple(*index, tuple);
        if (key.is_null()) continue;
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

    auto tree = UniquePtr<BPlusTree>(new BPlusTree(index->index_id, pool_.get()));
    tree->create();
    BPlusTree* tree_ptr = tree.get();
    index_trees_[index->index_id] = static_cast<UniquePtr<BPlusTree>&&>(tree);
    index->root_page_id = tree_ptr->root_page_id();

    HeapFile* heap = get_heap_file(index->table_id);
    PageId first_page = heap->first_data_page_id();
    if (first_page == kNullPageId) return;  // empty table, nothing to index
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
            Value key = index_key_from_tuple(*index, tuple);
            if (!key.is_null()) {
                tree_ptr->insert(key, RecordId(page_id, slot));
            }
        }
        pool_->unpin_page(page_id);
    }
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
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "next_txn_id=", 12) == 0) {
            next_txn = static_cast<u64>(std::strtoull(line + 12, nullptr, 10));
            found_next = next_txn > 0;
        }
    }
    std::fclose(f);
    if (found_next) {
        txn_manager_.ensure_next_txn_id_at_least(next_txn);
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
    pool_->flush_all();
    disk_mgr_->flush();
    // Sync table row counts for optimizer cost estimation
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
    flush();
    wal_->checkpoint();
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

    // Initialize per-column stats
    HashMap<String, bool> seen_values;
    for (u32 c = 0; c < num_cols; c++) {
        table->col_stats[c] = ColumnStats();
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

                // NDV: use string key for uniqueness
                char key_buf[32];
                snprintf(key_buf, sizeof(key_buf), "%u:%d", c,
                         val.type_id() == TypeId::kInt32 ? val.get_int32() : 0);
                String key(key_buf);
                if (!seen_values.find(key)) {
                    seen_values.insert(key, true);
                    stats.ndv++;
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
