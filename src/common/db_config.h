/**
 * @file db_config.h
 * @brief Runtime database configuration.
 */
#pragma once

#include "common/defs.h"
#include "common/config.h"
#include "container/string.h"

namespace minidb {

struct DbConfig {
    u64 shared_buffers_bytes = static_cast<u64>(kDefaultPoolFrames) * kPageSize;
    u64 work_mem_bytes = 16ULL * 1024ULL * 1024ULL;
    u64 query_memory_limit = 512ULL * 1024ULL * 1024ULL;
    u64 maintenance_work_mem_bytes = 256ULL * 1024ULL * 1024ULL;
    u64 temp_file_limit_bytes = 10ULL * 1024ULL * 1024ULL * 1024ULL;
    String temp_dir = String("/tmp");
    u64 max_result_rows = 1000000ULL;
    u64 max_result_bytes = 256ULL * 1024ULL * 1024ULL;
    u32 memory_pressure_threshold_percent = 85;

    u32 recovery_parallelism = 4;
    bool recover_indexes_lazy = true;
    bool startup_scan_txn_watermark = false;
    u64 checkpoint_timeout_ms = 60000;
    u64 checkpoint_wal_size_bytes = 256ULL * 1024ULL * 1024ULL;

    u64 wal_segment_size_bytes = 64ULL * 1024ULL * 1024ULL;
    u32 wal_keep_segments = 2;
    bool wal_fsync = true;
    bool wal_group_commit = true;
    u64 wal_group_commit_delay_ms = 2;

    u64 statement_timeout_ms = 30000;
    bool enable_hashjoin = true;
    bool enable_indexscan = true;
    bool enable_indexonlyscan = true;
    bool enable_parallel_seqscan = true;
    u32 parallel_workers = 4;
    u32 seqscan_prefetch_pages = 32;

    bool gc_enabled = true;
    u32 gc_ops_threshold = 10000;
    u32 gc_max_pages_per_cycle = 128;
    u64 gc_interval_ms = 5000;
    u32 deleted_tuple_ratio_threshold_percent = 20;

    String listen_addresses = String("127.0.0.1");
    u16 port = kDefaultPort;
    u32 max_connections = kMaxConnections;
    u32 max_active_queries = 64;
    u32 max_active_write_queries = 8;
    u32 max_active_transactions = 256;
    u32 admission_queue_size = 1024;
    u64 admission_queue_timeout_ms = 5000;
    u64 transaction_slot_wait_timeout_ms = 5000;
    u32 query_workers = 8;
    u32 io_workers = 2;
    u64 connection_idle_timeout_ms = 300000;
    u64 client_output_buffer_limit_bytes = 16ULL * 1024ULL * 1024ULL;
    u64 buffer_pool_wait_timeout_ms = 5000;
    u32 max_buffer_waiters = 1024;
    u32 buffer_pool_partitions = 16;
    u32 dirty_page_threshold_percent = 70;
    u32 background_flush_pages = 64;
    u32 checkpoint_flush_after = 128;
    u32 max_sql_size = 1024 * 1024;
    bool tcp_keepalive = true;

    bool doublewrite = true;
    bool page_checksum = true;
    u32 fd_cache_limit = 1024;
};

class DbConfigLoader {
public:
    static bool load_file(const String& path, DbConfig* config, String* error = nullptr);
    static String describe(const DbConfig& config);
};

} // namespace minidb
