#include "database/database.h"
#include "transaction/transaction.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace minidb;

static String make_temp_dir(const char* pattern) {
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "%s/%s",
                  std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp",
                  pattern);
    char* path = mkdtemp(tmpl);
    assert(path != nullptr);
    return String(path);
}

int main() {
    constexpr int kTxnCount = 280;
    DbConfig cfg;
    cfg.max_active_transactions = 320;
    cfg.transaction_slot_wait_timeout_ms = 1000;
    cfg.wal_fsync = false;
    cfg.gc_enabled = false;

    Database db(make_temp_dir("minidb-txn-slots.XXXXXX"), cfg);
    assert(db.txn_manager().txn_slot_count() >= static_cast<u32>(kTxnCount));

    std::atomic<int> started{0};
    std::atomic<int> failures{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    threads.reserve(kTxnCount);

    for (int i = 0; i < kTxnCount; i++) {
        threads.emplace_back([&]() {
            Transaction* txn = db.txn_manager().begin();
            if (!txn) {
                failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            started.fetch_add(1, std::memory_order_relaxed);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!db.txn_manager().commit(txn)) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (started.load(std::memory_order_relaxed) < kTxnCount &&
           failures.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    release.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    assert(started.load(std::memory_order_relaxed) == kTxnCount);
    assert(failures.load(std::memory_order_relaxed) == 0);
    assert(db.resources().snapshot().active_transactions == 0);
    return 0;
}
