/**
 * @file optional.h
 * @brief Custom Optional
 */
#pragma once

#include "container/utility.h"

namespace minidb {

template<typename T>
class Optional {
public:
    Optional() : has_value_(false) {}
    Optional(const T& value) : value_(value), has_value_(true) {}
    Optional(T&& value) : value_(static_cast<T&&>(value)), has_value_(true) {}

    Optional(const Optional& other) : has_value_(other.has_value_) {
        if (has_value_) new (&value_) T(other.value_);
    }

    Optional(Optional&& other) noexcept : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(static_cast<T&&>(other.value_));
            other.has_value_ = false;
        }
    }

    ~Optional() {
        if (has_value_) value_.~T();
    }

    Optional& operator=(const Optional& other) {
        if (this != &other) {
            if (has_value_) value_.~T();
            has_value_ = other.has_value_;
            if (has_value_) new (&value_) T(other.value_);
        }
        return *this;
    }

    Optional& operator=(Optional&& other) noexcept {
        if (this != &other) {
            if (has_value_) value_.~T();
            has_value_ = other.has_value_;
            if (has_value_) {
                new (&value_) T(static_cast<T&&>(other.value_));
                other.has_value_ = false;
            }
        }
        return *this;
    }

    bool has_value() const { return has_value_; }
    explicit operator bool() const { return has_value_; }

    T& value() { return value_; }
    const T& value() const { return value_; }

    T& operator*() { return value_; }
    const T& operator*() const { return value_; }

    T value_or(const T& default_val) const {
        return has_value_ ? value_ : default_val;
    }

private:
    union {
        T value_;
    };
    bool has_value_;
};

} // namespace minidb
