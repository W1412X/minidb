/**
 * @file resource_manager.h
 * @brief Global admission and resource accounting.
 */
#pragma once

#include "common/db_config.h"
#include "common/mutex.h"

namespace minidb {

struct ResourceSnapshot {
    u32 active_connections = 0;
    u32 active_queries = 0;
    u32 active_write_queries = 0;
    u32 waiting_queries = 0;
    u64 memory_reserved = 0;
    u64 temp_bytes_used = 0;
    u64 rejected_connections = 0;
    u64 rejected_queries = 0;
    u64 admission_timeouts = 0;
    u32 active_transactions = 0;
    u32 waiting_transactions = 0;
    u64 transaction_timeouts = 0;
};

class ResourceManager {
public:
    explicit ResourceManager(const DbConfig& config);

    bool acquire_connection();
    void release_connection();

    bool acquire_query(bool is_write, u64 memory_reservation);
    void release_query(bool is_write, u64 memory_reservation);
    bool acquire_transaction();
    void release_transaction();

    bool reserve_memory(u64 bytes);
    void release_memory(u64 bytes);
    bool reserve_temp(u64 bytes);
    void release_temp(u64 bytes);

    ResourceSnapshot snapshot() const;

private:
    DbConfig config_;
    mutable Mutex latch_;
    CondVar cond_;
    ResourceSnapshot stats_;
};

class ConnectionResourceGuard {
public:
    explicit ConnectionResourceGuard(ResourceManager& mgr);
    ~ConnectionResourceGuard();
    bool acquired() const { return acquired_; }

private:
    ResourceManager& mgr_;
    bool acquired_;
};

class QueryResourceGuard {
public:
    QueryResourceGuard(ResourceManager& mgr, bool is_write, u64 memory_reservation);
    ~QueryResourceGuard();
    bool acquired() const { return acquired_; }

private:
    ResourceManager& mgr_;
    bool is_write_;
    u64 memory_reservation_;
    bool acquired_;
};

} // namespace minidb
