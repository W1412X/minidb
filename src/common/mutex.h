/**
 * @file mutex.h
 * @brief Custom locks: Mutex, RwLock, LockGuard, ReadGuard, WriteGuard
 */
#pragma once

#include "noncopyable.h"
#include "defs.h"
#include <pthread.h>

namespace minidb {

class Mutex : NonCopyable {
public:
    Mutex();
    ~Mutex();

    void lock();
    bool try_lock();
    void unlock();

    pthread_mutex_t* native_handle() { return &mutex_; }

private:
    pthread_mutex_t mutex_;
};

class LockGuard {
public:
    explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& m_;
};

class CondVar : NonCopyable {
public:
    CondVar();
    ~CondVar();

    void wait(Mutex& mutex);
    bool timed_wait(Mutex& mutex, u32 timeout_ms);
    void signal();
    void broadcast();

private:
    pthread_cond_t cond_;
};

class RwLock : NonCopyable {
public:
    RwLock();
    ~RwLock();

    void read_lock();
    void read_unlock();
    void write_lock();
    void write_unlock();

private:
    pthread_rwlock_t rwlock_;
};

class ReadGuard {
public:
    explicit ReadGuard(RwLock& l) : l_(l) { l_.read_lock(); }
    ~ReadGuard() { l_.read_unlock(); }

    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;

private:
    RwLock& l_;
};

class WriteGuard {
public:
    explicit WriteGuard(RwLock& l) : l_(l) { l_.write_lock(); }
    ~WriteGuard() { l_.write_unlock(); }

    WriteGuard(const WriteGuard&) = delete;
    WriteGuard& operator=(const WriteGuard&) = delete;

private:
    RwLock& l_;
};

} // namespace minidb
