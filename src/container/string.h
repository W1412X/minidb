/**
 * @file string.h
 * @brief Custom String (SSO: Small String Optimization)
 */
#pragma once

#include "common/defs.h"
#include <cstring>
#include <cstdint>

namespace minidb {

class String {
public:
    using size_type = u32;
    static constexpr size_type kSSOCapacity = 22;
    static constexpr size_type npos = static_cast<size_type>(-1);

    // Constructor / Destructor
    String() noexcept;
    String(const char* cstr);
    String(const char* cstr, size_type length);
    String(size_type count, char ch);
    String(const String& other);
    String(String&& other) noexcept;
    explicit String(u32 value);     // Integer to String
    explicit String(u64 value);
    ~String();

    String& operator=(const String& other);
    String& operator=(String&& other) noexcept;
    String& operator=(const char* cstr);

    // Capacity
    size_type size() const noexcept { return size_; }
    size_type length() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    size_type capacity() const noexcept;

    // Element access
    char operator[](size_type idx) const;
    char& operator[](size_type idx);
    const char* c_str() const noexcept;

    // Modify
    String& operator+=(const String& other);
    String& operator+=(const char* cstr);
    String& operator+=(char ch);
    void clear() noexcept;
    void reserve(size_type new_capacity);

    // String concatenation
    String operator+(const String& other) const;
    String operator+(const char* cstr) const;
    String operator+(char ch) const;

    // Compare
    bool operator==(const String& other) const;
    bool operator!=(const String& other) const;
    bool operator<(const String& other) const;
    bool operator>(const String& other) const;
    bool operator<=(const String& other) const;
    bool operator>=(const String& other) const;

    // Search
    size_type find(char ch, size_type pos = 0) const;
    size_type find(const String& str, size_type pos = 0) const;

    // Substring
    String substr(size_type pos, size_type count = npos) const;

    // Compare (for Value sorting)
    int compare(const String& other) const;

private:
    struct HeapStorage {
        char* heap_buf_;
        size_type capacity_;
    };

    bool is_sso() const { return is_sso_; }
    void destroy();
    void grow(size_type min_capacity);

    union {
        char stack_buf_[kSSOCapacity + 1];
        HeapStorage heap_;
    };
    size_type size_;
    bool is_sso_;
};

} // namespace minidb
