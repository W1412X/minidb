/**
 * @file unique_ptr.h
 * @brief Exclusive-ownership smart pointer
 */
#pragma once

#include <cstddef>

namespace minidb {

template<typename T>
class UniquePtr {
public:
    UniquePtr() noexcept : ptr_(nullptr) {}
    explicit UniquePtr(T* ptr) noexcept : ptr_(ptr) {}
    UniquePtr(std::nullptr_t) noexcept : ptr_(nullptr) {}
    ~UniquePtr() { destroy(ptr_); }

    // Move semantics.
    UniquePtr(UniquePtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    // Converting move constructor (derived → base)
    template<typename U>
    UniquePtr(UniquePtr<U>&& other) noexcept : ptr_(other.release()) {}

    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            destroy(ptr_);
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // Prohibit copy
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    // Access
    T* get() const noexcept { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // Free
    T* release() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

    void reset(T* ptr = nullptr) noexcept {
        T* old = ptr_;
        ptr_ = ptr;
        destroy(old);
    }

private:
    // Deleting a pointer to an incomplete type is undefined behavior: the
    // compiler cannot see the (possibly non-trivial) destructor and silently
    // skips it, leaking owned resources. Force a hard compile error at every
    // deletion site instead — matching std::unique_ptr's default_delete.
    static void destroy(T* p) noexcept {
        static_assert(sizeof(T) > 0, "cannot delete pointer to incomplete type");
        delete p;
    }

    T* ptr_;
};

// Factory function
template<typename T, typename... Args>
UniquePtr<T> make_unique(Args&&... args) {
    return UniquePtr<T>(new T(static_cast<Args&&>(args)...));
}

} // namespace minidb
