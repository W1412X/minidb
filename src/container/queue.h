/**
 * @file queue.h
 * @brief Custom circular Queue
 */
#pragma once

#include "common/defs.h"
#include <cstdlib>

namespace minidb {

template<typename T>
class Queue {
public:
    using size_type = u32;

    Queue() : data_(nullptr), head_(0), tail_(0), size_(0), capacity_(0) {}

    explicit Queue(size_type initial_capacity)
        : head_(0), tail_(0), size_(0), capacity_(initial_capacity) {
        data_ = static_cast<T*>(malloc(sizeof(T) * capacity_));
    }

    ~Queue() {
        clear();
        free(data_);
    }

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    Queue(Queue&& other) noexcept
        : data_(other.data_), head_(other.head_), tail_(other.tail_),
          size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.head_ = 0;
        other.tail_ = 0;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    void push(const T& value) {
        if (size_ == capacity_) grow();
        new (data_ + tail_) T(value);
        tail_ = (tail_ + 1) % capacity_;
        size_++;
    }

    void push(T&& value) {
        if (size_ == capacity_) grow();
        new (data_ + tail_) T(static_cast<T&&>(value));
        tail_ = (tail_ + 1) % capacity_;
        size_++;
    }

    void pop() {
        if (size_ > 0) {
            data_[head_].~T();
            head_ = (head_ + 1) % capacity_;
            size_--;
        }
    }

    T& front() { return data_[head_]; }
    const T& front() const { return data_[head_]; }

    T& back() { return data_[(tail_ - 1 + capacity_) % capacity_]; }
    const T& back() const { return data_[(tail_ - 1 + capacity_) % capacity_]; }

    bool empty() const noexcept { return size_ == 0; }
    size_type size() const noexcept { return size_; }

    void clear() {
        while (size_ > 0) pop();
    }

private:
    void grow() {
        size_type new_cap = capacity_ == 0 ? 16 : capacity_ * 2;
        T* new_data = static_cast<T*>(malloc(sizeof(T) * new_cap));
        for (size_type i = 0; i < size_; i++) {
            new (new_data + i) T(move(data_[(head_ + i) % capacity_]));
            data_[(head_ + i) % capacity_].~T();
        }
        free(data_);
        data_ = new_data;
        head_ = 0;
        tail_ = size_;
        capacity_ = new_cap;
    }

    T* data_;
    size_type head_;
    size_type tail_;
    size_type size_;
    size_type capacity_;
};

} // namespace minidb
