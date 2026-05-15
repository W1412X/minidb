#include "common/resource_manager.h"

namespace minidb {

ResourceManager::ResourceManager(const DbConfig& config) : config_(config), stats_() {}

bool ResourceManager::acquire_connection() {
    LockGuard guard(latch_);
    if (config_.max_connections != 0 && stats_.active_connections >= config_.max_connections) {
        stats_.rejected_connections++;
        return false;
    }
    stats_.active_connections++;
    return true;
}

void ResourceManager::release_connection() {
    LockGuard guard(latch_);
    if (stats_.active_connections > 0) stats_.active_connections--;
    cond_.broadcast();
}

bool ResourceManager::acquire_query(bool is_write, u64 memory_reservation) {
    latch_.lock();
    if (config_.admission_queue_size != 0 &&
        stats_.waiting_queries >= config_.admission_queue_size) {
        stats_.rejected_queries++;
        latch_.unlock();
        return false;
    }

    stats_.waiting_queries++;
    while (true) {
        bool query_ok = config_.max_active_queries == 0 ||
                        stats_.active_queries < config_.max_active_queries;
        bool write_ok = !is_write || config_.max_active_write_queries == 0 ||
                        stats_.active_write_queries < config_.max_active_write_queries;
        bool memory_ok = memory_reservation == 0 ||
                         config_.query_memory_limit == 0 ||
                         stats_.memory_reserved + memory_reservation <= config_.query_memory_limit;

        if (query_ok && write_ok && memory_ok) {
            stats_.waiting_queries--;
            stats_.active_queries++;
            if (is_write) stats_.active_write_queries++;
            stats_.memory_reserved += memory_reservation;
            latch_.unlock();
            return true;
        }

        if (!cond_.timed_wait(latch_, static_cast<u32>(config_.admission_queue_timeout_ms))) {
            stats_.waiting_queries--;
            stats_.admission_timeouts++;
            stats_.rejected_queries++;
            latch_.unlock();
            return false;
        }
    }
}

void ResourceManager::release_query(bool is_write, u64 memory_reservation) {
    LockGuard guard(latch_);
    if (stats_.active_queries > 0) stats_.active_queries--;
    if (is_write && stats_.active_write_queries > 0) stats_.active_write_queries--;
    if (memory_reservation > stats_.memory_reserved) stats_.memory_reserved = 0;
    else stats_.memory_reserved -= memory_reservation;
    cond_.broadcast();
}

bool ResourceManager::acquire_transaction() {
    latch_.lock();
    stats_.waiting_transactions++;
    while (true) {
        bool ok = config_.max_active_transactions == 0 ||
                  stats_.active_transactions < config_.max_active_transactions;
        if (ok) {
            stats_.waiting_transactions--;
            stats_.active_transactions++;
            latch_.unlock();
            return true;
        }
        if (!cond_.timed_wait(latch_, static_cast<u32>(config_.transaction_slot_wait_timeout_ms))) {
            stats_.waiting_transactions--;
            stats_.transaction_timeouts++;
            latch_.unlock();
            return false;
        }
    }
}

void ResourceManager::release_transaction() {
    LockGuard guard(latch_);
    if (stats_.active_transactions > 0) stats_.active_transactions--;
    cond_.broadcast();
}

bool ResourceManager::reserve_memory(u64 bytes) {
    LockGuard guard(latch_);
    if (config_.query_memory_limit != 0 &&
        stats_.memory_reserved + bytes > config_.query_memory_limit) {
        return false;
    }
    stats_.memory_reserved += bytes;
    return true;
}

void ResourceManager::release_memory(u64 bytes) {
    LockGuard guard(latch_);
    if (bytes > stats_.memory_reserved) stats_.memory_reserved = 0;
    else stats_.memory_reserved -= bytes;
    cond_.broadcast();
}

bool ResourceManager::reserve_temp(u64 bytes) {
    LockGuard guard(latch_);
    if (config_.temp_file_limit_bytes != 0 &&
        stats_.temp_bytes_used + bytes > config_.temp_file_limit_bytes) {
        return false;
    }
    stats_.temp_bytes_used += bytes;
    return true;
}

void ResourceManager::release_temp(u64 bytes) {
    LockGuard guard(latch_);
    if (bytes > stats_.temp_bytes_used) stats_.temp_bytes_used = 0;
    else stats_.temp_bytes_used -= bytes;
}

ResourceSnapshot ResourceManager::snapshot() const {
    LockGuard guard(latch_);
    return stats_;
}

ConnectionResourceGuard::ConnectionResourceGuard(ResourceManager& mgr)
    : mgr_(mgr), acquired_(mgr_.acquire_connection()) {}

ConnectionResourceGuard::~ConnectionResourceGuard() {
    if (acquired_) mgr_.release_connection();
}

QueryResourceGuard::QueryResourceGuard(ResourceManager& mgr, bool is_write, u64 memory_reservation)
    : mgr_(mgr), is_write_(is_write), memory_reservation_(memory_reservation),
      acquired_(mgr_.acquire_query(is_write, memory_reservation)) {}

QueryResourceGuard::~QueryResourceGuard() {
    if (acquired_) mgr_.release_query(is_write_, memory_reservation_);
}

} // namespace minidb
