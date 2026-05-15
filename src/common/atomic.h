/**
 * @file atomic.h
 * @brief Custom atomic operations
 */
#pragma once

#include "noncopyable.h"

namespace minidb {

template<typename T>
class Atomic {
public:
    Atomic() noexcept : value_(0) {}
    explicit Atomic(T value) noexcept : value_(value) {}

    T load() const noexcept {
        return __atomic_load_n(&value_, __ATOMIC_SEQ_CST);
    }

    void store(T value) noexcept {
        __atomic_store_n(&value_, value, __ATOMIC_SEQ_CST);
    }

    T fetch_add(T arg) noexcept {
        return __atomic_fetch_add(&value_, arg, __ATOMIC_SEQ_CST);
    }

    T fetch_sub(T arg) noexcept {
        return __atomic_fetch_sub(&value_, arg, __ATOMIC_SEQ_CST);
    }

    bool compare_exchange_strong(T& expected, T desired) noexcept {
        return __atomic_compare_exchange_n(
            &value_, &expected, desired,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

private:
    T value_;
};

} // namespace minidb
