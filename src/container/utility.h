/**
 * @file utility.h
 * @brief Pair, Move, Forward and other basic Utilities
 */
#pragma once

#include <cstddef>

namespace minidb {

// === Move ===
template<typename T>
T&& move(T& t) noexcept {
    return static_cast<T&&>(t);
}

// === Forward ===
// Note: This is a simplified forward. For perfect forwarding, use std::forward.
template<typename T>
T& forward(T& t) noexcept {
    return t;
}

template<typename T>
T&& forward(T&& t) noexcept {
    return static_cast<T&&>(t);
}

// === Pair ===
template<typename T1, typename T2>
struct Pair {
    T1 first;
    T2 second;

    Pair() = default;
    Pair(const T1& f, const T2& s) : first(f), second(s) {}
    Pair(T1&& f, T2&& s) : first(move(f)), second(move(s)) {}
    Pair(const T1& f, T2&& s) : first(f), second(static_cast<T2&&>(s)) {}
    Pair(T1&& f, const T2& s) : first(static_cast<T1&&>(f)), second(s) {}

    bool operator==(const Pair& other) const {
        return first == other.first && second == other.second;
    }
    bool operator!=(const Pair& other) const {
        return !(*this == other);
    }
    bool operator<(const Pair& other) const {
        if (first != other.first) return first < other.first;
        return second < other.second;
    }
};

// === Swap ===
template<typename T>
void swap(T& a, T& b) {
    T tmp = move(a);
    a = move(b);
    b = move(tmp);
}

} // namespace minidb
