#include "common/db_config.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace minidb {

static bool ascii_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char* e = s + std::strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = '\0';
    }
    return s;
}

static bool parse_bool(const char* s, bool* out) {
    if (ascii_ieq(s, "on") || ascii_ieq(s, "true") || std::strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (ascii_ieq(s, "off") || ascii_ieq(s, "false") || std::strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_u64_unit(const char* s, u64* out) {
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || end == s) return false;
    while (*end == ' ' || *end == '\t') end++;

    u64 mul = 1;
    if (*end != '\0') {
        if (ascii_ieq(end, "B")) mul = 1;
        else if (ascii_ieq(end, "KB")) mul = 1024ULL;
        else if (ascii_ieq(end, "MB")) mul = 1024ULL * 1024ULL;
        else if (ascii_ieq(end, "GB")) mul = 1024ULL * 1024ULL * 1024ULL;
        else if (ascii_ieq(end, "MS")) mul = 1;
        else if (ascii_ieq(end, "S")) mul = 1000ULL;
        else if (ascii_ieq(end, "MIN")) mul = 60ULL * 1000ULL;
        else return false;
    }
    *out = static_cast<u64>(v) * mul;
    return true;
}

static bool parse_percent(const char* s, u32* out) {
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 10);
    if (errno != 0 || end == s) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end == '%') end++;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0' || v > 100) return false;
    *out = static_cast<u32>(v);
    return true;
}

static bool set_error(String* error, u32 line, const char* key, const char* value) {
    if (error) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "invalid config at line %u: %s=%s",
                      line, key ? key : "", value ? value : "");
        *error = String(buf);
    }
    return false;
}

bool DbConfigLoader::load_file(const String& path, DbConfig* config, String* error) {
    if (!config) return false;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        if (errno == ENOENT) return true;
        if (error) *error = String("failed to open config file");
        return false;
    }

    char line[1024];
    u32 line_no = 0;
    while (std::fgets(line, sizeof(line), f)) {
        line_no++;
        char* hash = std::strchr(line, '#');
        if (hash) *hash = '\0';
        char* p = trim(line);
        if (*p == '\0') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) {
            std::fclose(f);
            return set_error(error, line_no, p, "");
        }
        *eq = '\0';
        char* key = trim(p);
        char* value = trim(eq + 1);

        u64 bytes = 0;
        u64 ms = 0;

        if (std::strcmp(key, "shared_buffers") == 0) {
            if (!parse_u64_unit(value, &config->shared_buffers_bytes)) goto bad;
        } else if (std::strcmp(key, "work_mem") == 0) {
            if (!parse_u64_unit(value, &config->work_mem_bytes)) goto bad;
        } else if (std::strcmp(key, "query_memory_limit") == 0) {
            if (!parse_u64_unit(value, &config->query_memory_limit)) goto bad;
        } else if (std::strcmp(key, "maintenance_work_mem") == 0) {
            if (!parse_u64_unit(value, &config->maintenance_work_mem_bytes)) goto bad;
        } else if (std::strcmp(key, "temp_file_limit") == 0) {
            if (!parse_u64_unit(value, &config->temp_file_limit_bytes)) goto bad;
        } else if (std::strcmp(key, "temp_dir") == 0) {
            config->temp_dir = value;
        } else if (std::strcmp(key, "max_result_rows") == 0) {
            if (!parse_u64_unit(value, &config->max_result_rows)) goto bad;
        } else if (std::strcmp(key, "max_result_bytes") == 0) {
            if (!parse_u64_unit(value, &config->max_result_bytes)) goto bad;
        } else if (std::strcmp(key, "memory_pressure_threshold") == 0) {
            if (!parse_percent(value, &config->memory_pressure_threshold_percent)) goto bad;
        } else if (std::strcmp(key, "recovery_parallelism") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->recovery_parallelism = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "recover_indexes") == 0) {
            config->recover_indexes_lazy = !ascii_ieq(value, "rebuild");
        } else if (std::strcmp(key, "startup_scan_txn_watermark") == 0) {
            if (!parse_bool(value, &config->startup_scan_txn_watermark)) goto bad;
        } else if (std::strcmp(key, "checkpoint_timeout") == 0) {
            if (!parse_u64_unit(value, &ms)) goto bad;
            config->checkpoint_timeout_ms = ms;
        } else if (std::strcmp(key, "checkpoint_wal_size") == 0) {
            if (!parse_u64_unit(value, &config->checkpoint_wal_size_bytes)) goto bad;
        } else if (std::strcmp(key, "wal_segment_size") == 0) {
            if (!parse_u64_unit(value, &config->wal_segment_size_bytes)) goto bad;
        } else if (std::strcmp(key, "wal_keep_segments") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->wal_keep_segments = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "wal_fsync") == 0) {
            if (!parse_bool(value, &config->wal_fsync)) goto bad;
        } else if (std::strcmp(key, "wal_group_commit") == 0) {
            if (!parse_bool(value, &config->wal_group_commit)) goto bad;
        } else if (std::strcmp(key, "wal_group_commit_delay") == 0) {
            if (!parse_u64_unit(value, &config->wal_group_commit_delay_ms)) goto bad;
        } else if (std::strcmp(key, "statement_timeout") == 0) {
            if (!parse_u64_unit(value, &config->statement_timeout_ms)) goto bad;
        } else if (std::strcmp(key, "enable_hashjoin") == 0) {
            if (!parse_bool(value, &config->enable_hashjoin)) goto bad;
        } else if (std::strcmp(key, "enable_indexscan") == 0) {
            if (!parse_bool(value, &config->enable_indexscan)) goto bad;
        } else if (std::strcmp(key, "enable_indexonlyscan") == 0) {
            if (!parse_bool(value, &config->enable_indexonlyscan)) goto bad;
        } else if (std::strcmp(key, "enable_parallel_seqscan") == 0) {
            if (!parse_bool(value, &config->enable_parallel_seqscan)) goto bad;
        } else if (std::strcmp(key, "parallel_workers") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->parallel_workers = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "seqscan_prefetch_pages") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->seqscan_prefetch_pages = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "gc_enabled") == 0) {
            if (!parse_bool(value, &config->gc_enabled)) goto bad;
        } else if (std::strcmp(key, "gc_ops_threshold") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->gc_ops_threshold = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "gc_max_pages_per_cycle") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->gc_max_pages_per_cycle = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "gc_interval") == 0) {
            if (!parse_u64_unit(value, &config->gc_interval_ms)) goto bad;
        } else if (std::strcmp(key, "deleted_tuple_ratio_threshold") == 0) {
            if (!parse_percent(value, &config->deleted_tuple_ratio_threshold_percent)) goto bad;
        } else if (std::strcmp(key, "listen_addresses") == 0) {
            config->listen_addresses = value;
        } else if (std::strcmp(key, "port") == 0) {
            if (!parse_u64_unit(value, &bytes) || bytes > 65535) goto bad;
            config->port = static_cast<u16>(bytes);
        } else if (std::strcmp(key, "max_connections") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->max_connections = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "max_active_queries") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->max_active_queries = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "max_active_write_queries") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->max_active_write_queries = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "max_active_transactions") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->max_active_transactions = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "admission_queue_size") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->admission_queue_size = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "admission_queue_timeout") == 0) {
            if (!parse_u64_unit(value, &config->admission_queue_timeout_ms)) goto bad;
        } else if (std::strcmp(key, "transaction_slot_wait_timeout") == 0) {
            if (!parse_u64_unit(value, &config->transaction_slot_wait_timeout_ms)) goto bad;
        } else if (std::strcmp(key, "query_workers") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->query_workers = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "io_workers") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->io_workers = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "connection_idle_timeout") == 0) {
            if (!parse_u64_unit(value, &config->connection_idle_timeout_ms)) goto bad;
        } else if (std::strcmp(key, "client_output_buffer_limit") == 0) {
            if (!parse_u64_unit(value, &config->client_output_buffer_limit_bytes)) goto bad;
        } else if (std::strcmp(key, "buffer_pool_wait_timeout") == 0) {
            if (!parse_u64_unit(value, &config->buffer_pool_wait_timeout_ms)) goto bad;
        } else if (std::strcmp(key, "max_buffer_waiters") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->max_buffer_waiters = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "buffer_pool_partitions") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->buffer_pool_partitions = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "dirty_page_threshold") == 0) {
            if (!parse_percent(value, &config->dirty_page_threshold_percent)) goto bad;
        } else if (std::strcmp(key, "background_flush_pages") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->background_flush_pages = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "checkpoint_flush_after") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->checkpoint_flush_after = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "max_sql_size") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->max_sql_size = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "tcp_keepalive") == 0) {
            if (!parse_bool(value, &config->tcp_keepalive)) goto bad;
        } else if (std::strcmp(key, "doublewrite") == 0) {
            if (!parse_bool(value, &config->doublewrite)) goto bad;
        } else if (std::strcmp(key, "page_checksum") == 0) {
            if (!parse_bool(value, &config->page_checksum)) goto bad;
        } else if (std::strcmp(key, "fd_cache_limit") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->fd_cache_limit = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "storage_mode") == 0) {
            if (!ascii_ieq(value, "local") && !ascii_ieq(value, "remote")) goto bad;
            config->storage_mode = ascii_ieq(value, "remote") ? String("remote") : String("local");
        } else if (std::strcmp(key, "page_server_dir") == 0) {
            config->page_server_dir = value;
        } else if (std::strcmp(key, "page_server_host") == 0) {
            config->page_server_host = value;
        } else if (std::strcmp(key, "page_server_port") == 0) {
            if (!parse_u64_unit(value, &bytes) || bytes > 65535) goto bad;
            config->page_server_port = static_cast<u16>(bytes);
        } else if (std::strcmp(key, "storage_read_only") == 0) {
            if (!parse_bool(value, &config->storage_read_only)) goto bad;
        } else if (std::strcmp(key, "storage_read_lsn") == 0) {
            if (!parse_u64_unit(value, &config->storage_read_lsn)) goto bad;
        } else if (std::strcmp(key, "page_server_replicas") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->page_server_replicas = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "remote_page_batch_size") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->remote_page_batch_size = static_cast<u32>(bytes == 0 ? 1 : bytes);
        } else if (std::strcmp(key, "remote_flush_batch_size") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->remote_flush_batch_size = static_cast<u32>(bytes == 0 ? 1 : bytes);
        } else if (std::strcmp(key, "remote_connect_timeout") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->remote_connect_timeout_ms = static_cast<u32>(bytes == 0 ? 1 : bytes);
        } else if (std::strcmp(key, "remote_io_timeout") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->remote_io_timeout_ms = static_cast<u32>(bytes == 0 ? 1 : bytes);
        } else if (std::strcmp(key, "remote_retry_count") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->remote_retry_count = static_cast<u32>(bytes);
        } else if (std::strcmp(key, "remote_max_connections") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->remote_max_connections = static_cast<u32>(bytes == 0 ? 1 : bytes);
        } else if (std::strcmp(key, "page_server_max_connections") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->page_server_max_connections = static_cast<u32>(bytes == 0 ? 1 : bytes);
        } else if (std::strcmp(key, "page_server_cached_versions_per_page") == 0) {
            if (!parse_u64_unit(value, &bytes)) goto bad;
            config->page_server_cached_versions_per_page = static_cast<u32>(bytes == 0 ? 1 : bytes);
        }
        continue;

bad:
        std::fclose(f);
        return set_error(error, line_no, key, value);
    }
    std::fclose(f);
    return true;
}

String DbConfigLoader::describe(const DbConfig& config) {
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "shared_buffers=%llu\nwork_mem=%llu\nquery_memory_limit=%llu\n"
        "maintenance_work_mem=%llu\n"
        "temp_file_limit=%llu\ntemp_dir=%s\nmax_result_rows=%llu\nmax_result_bytes=%llu\n"
        "statement_timeout=%llu\nstartup_scan_txn_watermark=%s\n"
        "enable_indexscan=%s\nenable_indexonlyscan=%s\nenable_parallel_seqscan=%s\nparallel_workers=%u\n"
        "wal_segment_size=%llu\nwal_keep_segments=%u\nwal_fsync=%s\n"
        "wal_group_commit=%s\nwal_group_commit_delay=%llu\n"
        "gc_enabled=%s\ngc_ops_threshold=%u\ngc_max_pages_per_cycle=%u\n"
        "port=%u\nmax_connections=%u\nmax_active_queries=%u\n"
        "max_active_write_queries=%u\nmax_active_transactions=%u\n"
        "admission_queue_size=%u\nadmission_queue_timeout=%llu\n"
        "transaction_slot_wait_timeout=%llu\nquery_workers=%u\nio_workers=%u\n"
        "connection_idle_timeout=%llu\nclient_output_buffer_limit=%llu\n"
        "buffer_pool_wait_timeout=%llu\nmax_buffer_waiters=%u\nbuffer_pool_partitions=%u\n"
        "dirty_page_threshold=%u\nbackground_flush_pages=%u\ncheckpoint_flush_after=%u\n"
        "max_sql_size=%u\n"
        "storage_mode=%s\npage_server_dir=%s\npage_server_host=%s\npage_server_port=%u\n"
        "storage_read_only=%s\nstorage_read_lsn=%llu\n"
        "page_server_replicas=%u\nremote_page_batch_size=%u\nremote_flush_batch_size=%u\n"
        "remote_connect_timeout=%llu\nremote_io_timeout=%llu\nremote_retry_count=%u\n"
        "remote_max_connections=%u\npage_server_max_connections=%u\n"
        "page_server_cached_versions_per_page=%u\n",
        static_cast<unsigned long long>(config.shared_buffers_bytes),
        static_cast<unsigned long long>(config.work_mem_bytes),
        static_cast<unsigned long long>(config.query_memory_limit),
        static_cast<unsigned long long>(config.maintenance_work_mem_bytes),
        static_cast<unsigned long long>(config.temp_file_limit_bytes),
        config.temp_dir.c_str(),
        static_cast<unsigned long long>(config.max_result_rows),
        static_cast<unsigned long long>(config.max_result_bytes),
        static_cast<unsigned long long>(config.statement_timeout_ms),
        config.startup_scan_txn_watermark ? "on" : "off",
        config.enable_indexscan ? "on" : "off",
        config.enable_indexonlyscan ? "on" : "off",
        config.enable_parallel_seqscan ? "on" : "off",
        config.parallel_workers,
        static_cast<unsigned long long>(config.wal_segment_size_bytes),
        config.wal_keep_segments,
        config.wal_fsync ? "on" : "off",
        config.wal_group_commit ? "on" : "off",
        static_cast<unsigned long long>(config.wal_group_commit_delay_ms),
        config.gc_enabled ? "on" : "off",
        config.gc_ops_threshold,
        config.gc_max_pages_per_cycle,
        config.port,
        config.max_connections,
        config.max_active_queries,
        config.max_active_write_queries,
        config.max_active_transactions,
        config.admission_queue_size,
        static_cast<unsigned long long>(config.admission_queue_timeout_ms),
        static_cast<unsigned long long>(config.transaction_slot_wait_timeout_ms),
        config.query_workers,
        config.io_workers,
        static_cast<unsigned long long>(config.connection_idle_timeout_ms),
        static_cast<unsigned long long>(config.client_output_buffer_limit_bytes),
        static_cast<unsigned long long>(config.buffer_pool_wait_timeout_ms),
        config.max_buffer_waiters,
        config.buffer_pool_partitions,
        config.dirty_page_threshold_percent,
        config.background_flush_pages,
        config.checkpoint_flush_after,
        config.max_sql_size,
        config.storage_mode.c_str(),
        config.page_server_dir.c_str(),
        config.page_server_host.c_str(),
        config.page_server_port,
        config.storage_read_only ? "on" : "off",
        static_cast<unsigned long long>(config.storage_read_lsn),
        config.page_server_replicas,
        config.remote_page_batch_size,
        config.remote_flush_batch_size,
        static_cast<unsigned long long>(config.remote_connect_timeout_ms),
        static_cast<unsigned long long>(config.remote_io_timeout_ms),
        config.remote_retry_count,
        config.remote_max_connections,
        config.page_server_max_connections,
        config.page_server_cached_versions_per_page);
    return String(buf);
}

} // namespace minidb
