#include "concurrency/lock_manager.h"
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>

using namespace minidb;

struct ThreadArg {
    LockManager* lm;
    u64 txn_id;
    u32 table_a;
    u32 table_b;
    LockMode mode_a;
    LockMode mode_b;
    bool ok;
    long long wait_ms;
};

static long long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

static void* wait_lock_thread(void* raw) {
    auto* arg = static_cast<ThreadArg*>(raw);
    long long start = now_ms();
    Status st = arg->lm->lock_table(arg->txn_id, arg->table_a, arg->mode_a);
    arg->wait_ms = now_ms() - start;
    arg->ok = st.ok();
    return nullptr;
}

static void* deadlock_thread(void* raw) {
    auto* arg = static_cast<ThreadArg*>(raw);
    Status first = arg->lm->lock_table(arg->txn_id, arg->table_a, arg->mode_a);
    if (!first.ok()) {
        arg->ok = false;
        return nullptr;
    }
    usleep(100000);
    Status second = arg->lm->lock_table(arg->txn_id, arg->table_b, arg->mode_b);
    arg->ok = second.ok();
    arg->lm->unlock_all(arg->txn_id);
    return nullptr;
}

int main() {
    LockManager lm;

    // wait / wakeup
    if (!lm.lock_table(1, 42, LockMode::kRowExclusive).ok()) {
        std::fprintf(stderr, "failed to acquire initial lock\n");
        return 1;
    }
    ThreadArg wait_arg{&lm, 2, 42, 0, LockMode::kExclusive, LockMode::kExclusive, false, 0};
    pthread_t waiter;
    pthread_create(&waiter, nullptr, wait_lock_thread, &wait_arg);
    usleep(250000);
    lm.unlock_all(1);
    pthread_join(waiter, nullptr);
    if (!wait_arg.ok || wait_arg.wait_ms < 150) {
        std::fprintf(stderr, "wait lock test failed, ok=%d wait_ms=%lld\n",
                     wait_arg.ok ? 1 : 0, wait_arg.wait_ms);
        return 1;
    }
    lm.unlock_all(2);

    // simple deadlock detection
    ThreadArg d1{&lm, 11, 100, 200, LockMode::kExclusive, LockMode::kExclusive, false, 0};
    ThreadArg d2{&lm, 12, 200, 100, LockMode::kExclusive, LockMode::kExclusive, false, 0};
    pthread_t t1;
    pthread_t t2;
    pthread_create(&t1, nullptr, deadlock_thread, &d1);
    pthread_create(&t2, nullptr, deadlock_thread, &d2);
    pthread_join(t1, nullptr);
    pthread_join(t2, nullptr);
    if (d1.ok == d2.ok) {
        std::fprintf(stderr, "deadlock test expected one winner and one victim, got %d %d\n",
                     d1.ok ? 1 : 0, d2.ok ? 1 : 0);
        return 1;
    }

    return 0;
}
