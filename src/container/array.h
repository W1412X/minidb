/**
 * @file array.h
 * @brief Custom fixed-size Array
 */
#pragma once

#include "common/defs.h"

namespace minidb {

template<typename T, u32 N>
class Array {
public:
    using size_type = u32;

    Array() = default;

    T& operator[](size_type idx) { return data_[idx]; }
    const T& operator[](size_type idx) const { return data_[idx]; }

    T& at(size_type idx) { return data_[idx]; }
    const T& at(size_type idx) const { return data_[idx]; }

    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }

    constexpr size_type size() const noexcept { return N; }
    constexpr bool empty() const noexcept { return N == 0; }

    T& front() { return data_[0]; }
    const T& front() const { return data_[0]; }
    T& back() { return data_[N - 1]; }
    const T& back() const { return data_[N - 1]; }

    T* begin() noexcept { return data_; }
    T* end() noexcept { return data_ + N; }
    const T* begin() const noexcept { return data_; }
    const T* end() const noexcept { return data_ + N; }

private:
    T data_[N];
};

} // namespace minidb
