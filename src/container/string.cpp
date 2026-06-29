/**
 * @file string.cpp
 * @brief String implementation
 */
#include "container/string.h"
#include <cstring>
#include <cstdlib>

namespace minidb {

String::String() noexcept : size_(0), is_sso_(true) {
    stack_buf_[0] = '\0';
}

String::String(const char* cstr) {
    size_type len = static_cast<size_type>(strlen(cstr));
    if (len <= kSSOCapacity) {
        is_sso_ = true;
        size_ = len;
        memcpy(stack_buf_, cstr, len);
        stack_buf_[len] = '\0';
    } else {
        is_sso_ = false;
        heap_.capacity_ = len + 1;
        heap_.heap_buf_ = static_cast<char*>(malloc(heap_.capacity_));
        memcpy(heap_.heap_buf_, cstr, len);
        heap_.heap_buf_[len] = '\0';
        size_ = len;
    }
}

String::String(const char* cstr, size_type length) {
    if (length <= kSSOCapacity) {
        is_sso_ = true;
        size_ = length;
        memcpy(stack_buf_, cstr, length);
        stack_buf_[length] = '\0';
    } else {
        is_sso_ = false;
        heap_.capacity_ = length + 1;
        heap_.heap_buf_ = static_cast<char*>(malloc(heap_.capacity_));
        memcpy(heap_.heap_buf_, cstr, length);
        heap_.heap_buf_[length] = '\0';
        size_ = length;
    }
}

String::String(size_type count, char ch) {
    if (count <= kSSOCapacity) {
        is_sso_ = true;
        size_ = count;
        memset(stack_buf_, ch, count);
        stack_buf_[count] = '\0';
    } else {
        is_sso_ = false;
        heap_.capacity_ = count + 1;
        heap_.heap_buf_ = static_cast<char*>(malloc(heap_.capacity_));
        memset(heap_.heap_buf_, ch, count);
        heap_.heap_buf_[count] = '\0';
        size_ = count;
    }
}

String::String(const String& other) : size_(other.size_), is_sso_(other.is_sso_) {
    if (is_sso_) {
        memcpy(stack_buf_, other.stack_buf_, size_ + 1);
    } else {
        heap_.capacity_ = other.heap_.capacity_;
        heap_.heap_buf_ = static_cast<char*>(malloc(heap_.capacity_));
        memcpy(heap_.heap_buf_, other.heap_.heap_buf_, size_ + 1);
    }
}

String::String(String&& other) noexcept
    : size_(other.size_), is_sso_(other.is_sso_) {
    if (is_sso_) {
        memcpy(stack_buf_, other.stack_buf_, size_ + 1);
    } else {
        heap_.capacity_ = other.heap_.capacity_;
        heap_.heap_buf_ = other.heap_.heap_buf_;
        other.heap_.heap_buf_ = nullptr;
        other.is_sso_ = true;
        other.size_ = 0;
        other.stack_buf_[0] = '\0';
    }
}

String::~String() {
    if (!is_sso_ && heap_.heap_buf_) {
        free(heap_.heap_buf_);
    }
}

String::String(u32 value) : size_(0), is_sso_(true) {
    char buf[16];
    int len = 0;
    if (value == 0) {
        buf[0] = '0';
        len = 1;
    } else {
        while (value > 0) {
            buf[len++] = '0' + (value % 10);
            value /= 10;
        }
        // Reverse
        for (int i = 0; i < len / 2; i++) {
            char tmp = buf[i];
            buf[i] = buf[len - 1 - i];
            buf[len - 1 - i] = tmp;
        }
    }
    buf[len] = '\0';
    *this = String(buf, static_cast<size_type>(len));
}

String::String(u64 value) : size_(0), is_sso_(true) {
    char buf[24];
    int len = 0;
    if (value == 0) {
        buf[0] = '0';
        len = 1;
    } else {
        while (value > 0) {
            buf[len++] = '0' + (value % 10);
            value /= 10;
        }
        for (int i = 0; i < len / 2; i++) {
            char tmp = buf[i];
            buf[i] = buf[len - 1 - i];
            buf[len - 1 - i] = tmp;
        }
    }
    buf[len] = '\0';
    *this = String(buf, static_cast<size_type>(len));
}

String& String::operator=(const String& other) {
    if (this != &other) {
        if (!is_sso_ && heap_.heap_buf_) free(heap_.heap_buf_);
        size_ = other.size_;
        is_sso_ = other.is_sso_;
        if (is_sso_) {
            memcpy(stack_buf_, other.stack_buf_, size_ + 1);
        } else {
            heap_.capacity_ = other.heap_.capacity_;
            heap_.heap_buf_ = static_cast<char*>(malloc(heap_.capacity_));
            memcpy(heap_.heap_buf_, other.heap_.heap_buf_, size_ + 1);
        }
    }
    return *this;
}

String& String::operator=(String&& other) noexcept {
    if (this != &other) {
        if (!is_sso_ && heap_.heap_buf_) free(heap_.heap_buf_);
        size_ = other.size_;
        is_sso_ = other.is_sso_;
        if (is_sso_) {
            memcpy(stack_buf_, other.stack_buf_, size_ + 1);
        } else {
            heap_.capacity_ = other.heap_.capacity_;
            heap_.heap_buf_ = other.heap_.heap_buf_;
            other.heap_.heap_buf_ = nullptr;
            other.is_sso_ = true;
            other.size_ = 0;
            other.stack_buf_[0] = '\0';
        }
    }
    return *this;
}

String& String::operator=(const char* cstr) {
    if (!is_sso_ && heap_.heap_buf_) free(heap_.heap_buf_);
    size_type len = static_cast<size_type>(strlen(cstr));
    if (len <= kSSOCapacity) {
        is_sso_ = true;
        size_ = len;
        memcpy(stack_buf_, cstr, len);
        stack_buf_[len] = '\0';
    } else {
        is_sso_ = false;
        heap_.capacity_ = len + 1;
        heap_.heap_buf_ = static_cast<char*>(malloc(heap_.capacity_));
        memcpy(heap_.heap_buf_, cstr, len);
        heap_.heap_buf_[len] = '\0';
        size_ = len;
    }
    return *this;
}

String::size_type String::capacity() const noexcept {
    return is_sso_ ? kSSOCapacity : heap_.capacity_;
}

char String::operator[](size_type idx) const {
    return is_sso_ ? stack_buf_[idx] : heap_.heap_buf_[idx];
}

char& String::operator[](size_type idx) {
    return is_sso_ ? stack_buf_[idx] : heap_.heap_buf_[idx];
}

const char* String::c_str() const noexcept {
    return is_sso_ ? stack_buf_ : heap_.heap_buf_;
}

String& String::operator+=(const String& other) {
    size_type new_size = size_ + other.size_;
    if (new_size <= kSSOCapacity) {
        memcpy(stack_buf_ + size_, other.c_str(), other.size_);
        size_ = new_size;
        stack_buf_[size_] = '\0';
    } else {
        if (is_sso_) {
            char tmp[kSSOCapacity + 1];
            memcpy(tmp, stack_buf_, size_);
            grow(new_size + 1);
            memcpy(heap_.heap_buf_, tmp, size_);
            memcpy(heap_.heap_buf_ + size_, other.c_str(), other.size_);
        } else {
            if (new_size + 1 > heap_.capacity_) grow(new_size + 1);
            memcpy(heap_.heap_buf_ + size_, other.c_str(), other.size_);
        }
        is_sso_ = false;
        size_ = new_size;
        heap_.heap_buf_[size_] = '\0';
    }
    return *this;
}

String& String::operator+=(const char* cstr) {
    String tmp(cstr);
    return *this += tmp;
}

String& String::operator+=(char ch) {
    if (size_ + 1 <= kSSOCapacity) {
        stack_buf_[size_] = ch;
        size_++;
        stack_buf_[size_] = '\0';
    } else {
        String tmp(&ch, 1);
        return *this += tmp;
    }
    return *this;
}

void String::clear() noexcept {
    if (!is_sso_ && heap_.heap_buf_) {
        free(heap_.heap_buf_);
        is_sso_ = true;
    }
    size_ = 0;
    stack_buf_[0] = '\0';
}

void String::reserve(size_type new_capacity) {
    if (new_capacity > kSSOCapacity && !is_sso_) {
        if (new_capacity > heap_.capacity_) grow(new_capacity);
    } else if (new_capacity > kSSOCapacity && is_sso_) {
        grow(new_capacity);
    }
}

String String::operator+(const String& other) const {
    String result(*this);
    result += other;
    return result;
}

String String::operator+(const char* cstr) const {
    String result(*this);
    result += cstr;
    return result;
}

String String::operator+(char ch) const {
    String result(*this);
    result += ch;
    return result;
}

int String::compare(const String& other) const {
    u32 min_len = (size_ < other.size_) ? size_ : other.size_;
    int result = std::memcmp(c_str(), other.c_str(), min_len);
    if (result != 0) return result;
    if (size_ < other.size_) return -1;
    if (size_ > other.size_) return 1;
    return 0;
}

bool String::operator==(const String& other) const {
    if (size_ != other.size_) return false;
    return memcmp(c_str(), other.c_str(), size_) == 0;
}

bool String::operator!=(const String& other) const { return !(*this == other); }
// Relational operators must use lexicographic compare(), which applies the
// length tiebreak after the common prefix matches. A bare memcmp over only
// min(size) bytes wrongly reports "abc" and "abcd" as equal-ordering, breaking
// sorts and ordered comparisons whenever one string is a prefix of another.
bool String::operator<(const String& other) const { return compare(other) < 0; }
bool String::operator>(const String& other) const { return compare(other) > 0; }
bool String::operator<=(const String& other) const { return compare(other) <= 0; }
bool String::operator>=(const String& other) const { return compare(other) >= 0; }

String::size_type String::find(char ch, size_type pos) const {
    for (size_type i = pos; i < size_; i++) {
        if (c_str()[i] == ch) return i;
    }
    return npos;
}

String::size_type String::find(const String& str, size_type pos) const {
    if (str.size_ == 0) return pos;
    if (str.size_ > size_) return npos;
    for (size_type i = pos; i <= size_ - str.size_; i++) {
        if (memcmp(c_str() + i, str.c_str(), str.size_) == 0) return i;
    }
    return npos;
}

String String::substr(size_type pos, size_type count) const {
    if (pos >= size_) return String();
    if (count == npos || pos + count > size_) count = size_ - pos;
    return String(c_str() + pos, count);
}

void String::grow(size_type min_capacity) {
    size_type new_cap = is_sso_ ? 64 : heap_.capacity_;
    while (new_cap < min_capacity) new_cap *= 2;

    char* new_buf = static_cast<char*>(malloc(new_cap));
    memcpy(new_buf, c_str(), size_);
    new_buf[size_] = '\0';

    if (!is_sso_ && heap_.heap_buf_) free(heap_.heap_buf_);
    heap_.heap_buf_ = new_buf;
    heap_.capacity_ = new_cap;
    is_sso_ = false;
}

} // namespace minidb
