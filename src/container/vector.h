/**
 * @file vector.h
 * @brief Custom dynamic Array
 */
#pragma once

#include "common/defs.h"
#include "container/utility.h"
#include <cstring>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace minidb {

template<typename T>
class Vector {
public:
    using size_type = u32;
    using iterator = T*;
    using const_iterator = const T*;

    Vector() noexcept : data_(nullptr), size_(0), capacity_(0) {}

    explicit Vector(size_type count) : size_(count), capacity_(count) {
        data_ = static_cast<T*>(malloc(sizeof(T) * count));
        for (size_type i = 0; i < count; i++) {
            new (data_ + i) T();
        }
    }

    Vector(size_type count, const T& value)
        : size_(count), capacity_(count) {
        data_ = static_cast<T*>(malloc(sizeof(T) * count));
        for (size_type i = 0; i < count; i++) {
            new (data_ + i) T(value);
        }
    }

    Vector(const Vector& other) : size_(other.size_), capacity_(other.capacity_) {
        if (capacity_ > 0) {
            data_ = static_cast<T*>(malloc(sizeof(T) * capacity_));
            for (size_type i = 0; i < size_; i++) {
                new (data_ + i) T(other.data_[i]);
            }
        } else {
            data_ = nullptr;
        }
    }

    Vector(Vector&& other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    ~Vector() {
        clear();
        free(data_);
    }

    Vector& operator=(const Vector& other) {
        if (this != &other) {
            clear();
            free(data_);
            size_ = other.size_;
            capacity_ = other.capacity_;
            if (capacity_ > 0) {
                data_ = static_cast<T*>(malloc(sizeof(T) * capacity_));
                for (size_type i = 0; i < size_; i++) {
                    new (data_ + i) T(other.data_[i]);
                }
            } else {
                data_ = nullptr;
            }
        }
        return *this;
    }

    Vector& operator=(Vector&& other) noexcept {
        if (this != &other) {
            clear();
            free(data_);
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    // Capacity
    size_type size() const noexcept { return size_; }
    size_type capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size_ == 0; }

    void reserve(size_type new_capacity) {
        if (new_capacity > capacity_) {
            grow(new_capacity);
        }
    }

    void resize(size_type new_size) {
        if (new_size > capacity_) {
            grow(new_size);
        }
        if (new_size > size_) {
            for (size_type i = size_; i < new_size; i++) {
                new (data_ + i) T();
            }
        } else if (new_size < size_) {
            for (size_type i = new_size; i < size_; i++) {
                data_[i].~T();
            }
        }
        size_ = new_size;
    }

    // Element access
    T& operator[](size_type idx) { return data_[idx]; }
    const T& operator[](size_type idx) const { return data_[idx]; }

    T& at(size_type idx) { return data_[idx]; }
    const T& at(size_type idx) const { return data_[idx]; }

    T& front() { return data_[0]; }
    const T& front() const { return data_[0]; }

    T& back() { return data_[size_ - 1]; }
    const T& back() const { return data_[size_ - 1]; }

    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }

    // Modify
    void push_back(const T& value) {
        if (size_ == capacity_) {
            grow(capacity_ == 0 ? 16 : capacity_ * 2);
        }
        new (data_ + size_) T(value);
        size_++;
    }

    void push_back(T&& value) {
        if (size_ == capacity_) {
            grow(capacity_ == 0 ? 16 : capacity_ * 2);
        }
        new (data_ + size_) T(move(value));
        size_++;
    }

    template<typename... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == capacity_) {
            grow(capacity_ == 0 ? 16 : capacity_ * 2);
        }
        new (data_ + size_) T(forward<Args>(args)...);
        T& result = data_[size_];
        size_++;
        return result;
    }

    void pop_back() {
        if (size_ > 0) {
            size_--;
            data_[size_].~T();
        }
    }

    void clear() noexcept {
        for (size_type i = 0; i < size_; i++) {
            data_[i].~T();
        }
        size_ = 0;
    }

    iterator insert(iterator pos, const T& value) {
        size_type offset = pos - data_;
        if (size_ == capacity_) {
            grow(capacity_ == 0 ? 16 : capacity_ * 2);
        }
        pos = data_ + offset;
        for (size_type i = size_; i > offset; i--) {
            new (data_ + i) T(move(data_[i - 1]));
            data_[i - 1].~T();
        }
        new (pos) T(value);
        size_++;
        return pos;
    }

    iterator erase(iterator pos) {
        size_type offset = pos - data_;
        data_[offset].~T();
        for (size_type i = offset; i < size_ - 1; i++) {
            new (data_ + i) T(move(data_[i + 1]));
            data_[i + 1].~T();
        }
        size_--;
        return data_ + offset;
    }

    iterator erase(iterator first, iterator last) {
        size_type start = first - data_;
        size_type end = last - data_;
        for (size_type i = start; i < end; i++) {
            data_[i].~T();
        }
        for (size_type i = end; i < size_; i++) {
            new (data_ + start + (i - end)) T(move(data_[i]));
            data_[i].~T();
        }
        size_ -= (end - start);
        return data_ + start;
    }

    // Iterator
    iterator begin() noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }

    // Search
    iterator find(const T& value) {
        for (size_type i = 0; i < size_; i++) {
            if (data_[i] == value) return data_ + i;
        }
        return end();
    }

    bool contains(const T& value) const {
        for (size_type i = 0; i < size_; i++) {
            if (data_[i] == value) return true;
        }
        return false;
    }

    // Max-heap operations for Top-N optimization
    void push_heap() {
        i64 i = static_cast<i64>(size_) - 1;
        while (i > 0) {
            i64 p = (i - 1) / 2;
            if (data_[p] < data_[i]) {
                swap(data_[p], data_[i]);
                i = p;
            } else break;
        }
    }

    template<typename Compare>
    void push_heap(Compare comp) {
        i64 i = static_cast<i64>(size_) - 1;
        while (i > 0) {
            i64 p = (i - 1) / 2;
            if (comp(data_[p], data_[i])) {
                swap(data_[p], data_[i]);
                i = p;
            } else break;
        }
    }

    void pop_heap() {
        if (size_ <= 1) return;
        swap(data_[0], data_[size_ - 1]);
        sift_down(static_cast<i64>(0), static_cast<i64>(0), static_cast<i64>(size_ - 1));
    }

    template<typename Compare>
    void pop_heap(Compare comp) {
        if (size_ <= 1) return;
        swap(data_[0], data_[size_ - 1]);
        sift_down(static_cast<i64>(0), static_cast<i64>(0), static_cast<i64>(size_ - 1), comp);
    }

    void make_heap() {
        if (size_ <= 1) return;
        i64 n = static_cast<i64>(size_);
        for (i64 i = n / 2 - 1; i >= 0; i--) {
            sift_down(static_cast<i64>(0), i, n);
        }
    }

    template<typename Compare>
    void make_heap(Compare comp) {
        if (size_ <= 1) return;
        i64 n = static_cast<i64>(size_);
        for (i64 i = n / 2 - 1; i >= 0; i--) {
            sift_down(static_cast<i64>(0), i, n, comp);
        }
    }

    // Sort (intro sort: quick sort + heap sort + insertion sort)
    void sort() {
        if (size_ <= 1) return;
        introsort(0, static_cast<i64>(size_) - 1, 0);
    }

    template<typename Compare>
    void sort(Compare comp) {
        if (size_ <= 1) return;
        introsort(0, static_cast<i64>(size_) - 1, 0, comp);
    }

private:
    void grow(size_type min_capacity) {
        size_type new_cap = capacity_ == 0 ? 16 : capacity_ * 2;
        while (new_cap < min_capacity) new_cap *= 2;

        T* new_data = static_cast<T*>(malloc(sizeof(T) * new_cap));
        for (size_type i = 0; i < size_; i++) {
            new (new_data + i) T(move(data_[i]));
            data_[i].~T();
        }
        free(data_);
        data_ = new_data;
        capacity_ = new_cap;
    }

    void introsort(i64 lo, i64 hi, i64 depth_limit) {
        if (hi - lo <= 15) {
            insertion_sort(lo, hi);
            return;
        }
        if (depth_limit == 0) {
            heap_sort(lo, hi);
            return;
        }
        i64 p = partition(lo, hi);
        introsort(lo, p - 1, depth_limit - 1);
        introsort(p + 1, hi, depth_limit - 1);
    }

    template<typename Compare>
    void introsort(i64 lo, i64 hi, i64 depth_limit, Compare comp) {
        if (hi - lo <= 15) {
            insertion_sort(lo, hi, comp);
            return;
        }
        if (depth_limit == 0) {
            heap_sort(lo, hi, comp);
            return;
        }
        i64 p = partition(lo, hi, comp);
        introsort(lo, p - 1, depth_limit - 1, comp);
        introsort(p + 1, hi, depth_limit - 1, comp);
    }

    i64 partition(i64 lo, i64 hi) {
        T pivot = data_[hi];
        i64 i = lo - 1;
        for (i64 j = lo; j < hi; j++) {
            if (data_[j] < pivot) {
                i++;
                swap(data_[i], data_[j]);
            }
        }
        swap(data_[i + 1], data_[hi]);
        return i + 1;
    }

    template<typename Compare>
    i64 partition(i64 lo, i64 hi, Compare comp) {
        T pivot = data_[hi];
        i64 i = lo - 1;
        for (i64 j = lo; j < hi; j++) {
            if (comp(data_[j], pivot)) {
                i++;
                swap(data_[i], data_[j]);
            }
        }
        swap(data_[i + 1], data_[hi]);
        return i + 1;
    }

    void insertion_sort(i64 lo, i64 hi) {
        for (i64 i = lo + 1; i <= hi; i++) {
            T key = move(data_[i]);
            i64 j = i - 1;
            while (j >= lo && data_[j] > key) {
                data_[j + 1] = move(data_[j]);
                j--;
            }
            data_[j + 1] = move(key);
        }
    }

    template<typename Compare>
    void insertion_sort(i64 lo, i64 hi, Compare comp) {
        for (i64 i = lo + 1; i <= hi; i++) {
            T key = move(data_[i]);
            i64 j = i - 1;
            while (j >= lo && comp(key, data_[j])) {
                data_[j + 1] = move(data_[j]);
                j--;
            }
            data_[j + 1] = move(key);
        }
    }

    void heap_sort(i64 lo, i64 hi) {
        i64 n = hi - lo + 1;
        for (i64 i = n / 2 - 1; i >= 0; i--) {
            sift_down(lo, i, n);
        }
        for (i64 i = n - 1; i > 0; i--) {
            swap(data_[lo], data_[lo + i]);
            sift_down(lo, 0, i);
        }
    }

    template<typename Compare>
    void heap_sort(i64 lo, i64 hi, Compare comp) {
        i64 n = hi - lo + 1;
        for (i64 i = n / 2 - 1; i >= 0; i--) {
            sift_down(lo, i, n, comp);
        }
        for (i64 i = n - 1; i > 0; i--) {
            swap(data_[lo], data_[lo + i]);
            sift_down(lo, 0, i, comp);
        }
    }

    void sift_down(i64 lo, i64 root, i64 n) {
        while (true) {
            i64 child = 2 * root + 1;
            if (child >= n) break;
            if (child + 1 < n && data_[lo + child] < data_[lo + child + 1]) {
                child++;
            }
            if (data_[lo + root] < data_[lo + child]) {
                swap(data_[lo + root], data_[lo + child]);
                root = child;
            } else {
                break;
            }
        }
    }

    template<typename Compare>
    void sift_down(i64 lo, i64 root, i64 n, Compare comp) {
        while (true) {
            i64 child = 2 * root + 1;
            if (child >= n) break;
            if (child + 1 < n && comp(data_[lo + child], data_[lo + child + 1])) {
                child++;
            }
            if (comp(data_[lo + root], data_[lo + child])) {
                swap(data_[lo + root], data_[lo + child]);
                root = child;
            } else {
                break;
            }
        }
    }

    T* data_;
    size_type size_;
    size_type capacity_;
};

} // namespace minidb
