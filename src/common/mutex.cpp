/**
 * @file mutex.cpp
 * @brief Mutex / RwLock implementation
 */
#include "common/mutex.h"
#include <ctime>

namespace minidb {

// ============================================================
// Mutex
// ============================================================

Mutex::Mutex() {
    pthread_mutex_init(&mutex_, nullptr);
}

Mutex::~Mutex() {
    pthread_mutex_destroy(&mutex_);
}

void Mutex::lock() {
    pthread_mutex_lock(&mutex_);
}

bool Mutex::try_lock() {
    return pthread_mutex_trylock(&mutex_) == 0;
}

void Mutex::unlock() {
    pthread_mutex_unlock(&mutex_);
}

// ============================================================
// CondVar
// ============================================================

CondVar::CondVar() {
    pthread_cond_init(&cond_, nullptr);
}

CondVar::~CondVar() {
    pthread_cond_destroy(&cond_);
}

void CondVar::wait(Mutex& mutex) {
    pthread_cond_wait(&cond_, mutex.native_handle());
}

bool CondVar::timed_wait(Mutex& mutex, u32 timeout_ms) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += static_cast<time_t>(timeout_ms / 1000);
    ts.tv_nsec += static_cast<long>((timeout_ms % 1000) * 1000000ULL);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&cond_, mutex.native_handle(), &ts) == 0;
}

void CondVar::signal() {
    pthread_cond_signal(&cond_);
}

void CondVar::broadcast() {
    pthread_cond_broadcast(&cond_);
}

// ============================================================
// RwLock
// ============================================================

RwLock::RwLock() {
    pthread_rwlock_init(&rwlock_, nullptr);
}

RwLock::~RwLock() {
    pthread_rwlock_destroy(&rwlock_);
}

void RwLock::read_lock() {
    pthread_rwlock_rdlock(&rwlock_);
}

void RwLock::read_unlock() {
    pthread_rwlock_unlock(&rwlock_);
}

void RwLock::write_lock() {
    pthread_rwlock_wrlock(&rwlock_);
}

void RwLock::write_unlock() {
    pthread_rwlock_unlock(&rwlock_);
}

} // namespace minidb
