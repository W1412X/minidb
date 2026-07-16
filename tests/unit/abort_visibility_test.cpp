/**
 * C1 regression: after a transaction aborts, concurrent readers must never
 * treat its xid as a committed xmin. Exercises the early status_log publish
 * in rollback() plus is_visible()'s uncommitted check under load.
 */
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
    DbConfig cfg;
    cfg.max_active_transactions = 64;
    cfg.transaction_slot_wait_timeout_ms = 1000;
    cfg.wal_fsync = false;
    cfg.gc_enabled = false;

    Database db(make_temp_dir("minidb-abort-vis.XXXXXX"), cfg);

    std::atomic<bool> stop{false};
    std::atomic<u64> latest_aborted{0};
    std::atomic<int> visible_bugs{0};
    std::atomic<int> aborts_done{0};

    std::thread writer([&]() {
        for (int i = 0; i < 200; i++) {
            Transaction* txn = db.txn_manager().begin();
            if (!txn) continue;
            u64 xid = txn->id();
            assert(db.txn_manager().rollback(txn));
            latest_aborted.store(xid, std::memory_order_release);
            aborts_done.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; r++) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire) ||
                   latest_aborted.load(std::memory_order_acquire) == 0) {
                u64 xid = latest_aborted.load(std::memory_order_acquire);
                if (xid == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    continue;
                }
                Transaction* reader = db.txn_manager().begin();
                if (!reader) continue;
                if (db.txn_manager().is_visible(xid, kInvalidTxnId, *reader)) {
                    visible_bugs.fetch_add(1, std::memory_order_relaxed);
                }
                assert(db.txn_manager().rollback(reader));
            }
            // Final check against the last aborted xid.
            u64 xid = latest_aborted.load(std::memory_order_acquire);
            Transaction* reader = db.txn_manager().begin();
            if (reader) {
                if (db.txn_manager().is_visible(xid, kInvalidTxnId, *reader)) {
                    visible_bugs.fetch_add(1, std::memory_order_relaxed);
                }
                assert(db.txn_manager().rollback(reader));
            }
        });
    }

    writer.join();
    for (auto& t : readers) t.join();

    assert(aborts_done.load(std::memory_order_relaxed) > 0);
    assert(visible_bugs.load(std::memory_order_relaxed) == 0);
    return 0;
}
