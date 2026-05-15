/**
 * @file linked_list.h
 * @brief Custom doubly-linked list
 */
#pragma once

#include "common/defs.h"

namespace minidb {

template<typename T>
class LinkedList {
public:
    struct Node {
        T data;
        Node* prev;
        Node* next;
        Node() : data(), prev(nullptr), next(nullptr) {}
        explicit Node(const T& val) : data(val), prev(nullptr), next(nullptr) {}
        explicit Node(T&& val) : data(static_cast<T&&>(val)), prev(nullptr), next(nullptr) {}
    };

    using size_type = u32;

    LinkedList() : head_(nullptr), tail_(nullptr), size_(0) {}
    ~LinkedList() { clear(); }

    LinkedList(const LinkedList&) = delete;
    LinkedList& operator=(const LinkedList&) = delete;

    LinkedList(LinkedList&& other) noexcept
        : head_(other.head_), tail_(other.tail_), size_(other.size_) {
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.size_ = 0;
    }

    // Head operations
    void push_front(const T& value) {
        Node* node = new Node(value);
        node->next = head_;
        if (head_) head_->prev = node;
        head_ = node;
        if (!tail_) tail_ = node;
        size_++;
    }

    void push_front(T&& value) {
        Node* node = new Node(static_cast<T&&>(value));
        node->next = head_;
        if (head_) head_->prev = node;
        head_ = node;
        if (!tail_) tail_ = node;
        size_++;
    }

    void pop_front() {
        if (!head_) return;
        Node* old = head_;
        head_ = head_->next;
        if (head_) head_->prev = nullptr;
        else tail_ = nullptr;
        delete old;
        size_--;
    }

    // Tail operations
    void push_back(const T& value) {
        Node* node = new Node(value);
        node->prev = tail_;
        if (tail_) tail_->next = node;
        tail_ = node;
        if (!head_) head_ = node;
        size_++;
    }

    void push_back(T&& value) {
        Node* node = new Node(static_cast<T&&>(value));
        node->prev = tail_;
        if (tail_) tail_->next = node;
        tail_ = node;
        if (!head_) head_ = node;
        size_++;
    }

    /// Append and return the node pointer (O(1)); used by BufferPool LRU bookkeeping.
    Node* push_back_link(const T& value) {
        Node* node = new Node(value);
        node->prev = tail_;
        if (tail_) tail_->next = node;
        tail_ = node;
        if (!head_) head_ = node;
        size_++;
        return node;
    }

    /// Move an existing node to the front without allocating (O(1)).
    void move_node_to_front(Node* node) {
        if (!node || head_ == node) return;
        if (node->prev) node->prev->next = node->next;
        else head_ = node->next;
        if (node->next) node->next->prev = node->prev;
        else tail_ = node->prev;
        node->prev = nullptr;
        node->next = head_;
        if (head_) head_->prev = node;
        head_ = node;
    }

    /// Move an existing node to the back without allocating (O(1)).
    void move_node_to_back(Node* node) {
        if (!node || tail_ == node) return;
        if (node->prev) node->prev->next = node->next;
        else head_ = node->next;
        if (node->next) node->next->prev = node->prev;
        else tail_ = node->prev;
        node->next = nullptr;
        node->prev = tail_;
        if (tail_) tail_->next = node;
        tail_ = node;
        if (!head_) head_ = node;
    }

    void pop_back() {
        if (!tail_) return;
        Node* old = tail_;
        tail_ = tail_->prev;
        if (tail_) tail_->next = nullptr;
        else head_ = nullptr;
        delete old;
        size_--;
    }

    // Access
    T& front() { return head_->data; }
    const T& front() const { return head_->data; }
    T& back() { return tail_->data; }
    const T& back() const { return tail_->data; }

    bool empty() const noexcept { return size_ == 0; }
    size_type size() const noexcept { return size_; }

    void clear() {
        Node* current = head_;
        while (current) {
            Node* next = current->next;
            delete current;
            current = next;
        }
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    // Search
    Node* find(const T& value) {
        Node* current = head_;
        while (current) {
            if (current->data == value) return current;
            current = current->next;
        }
        return nullptr;
    }

    // Remove specified node
    void remove_node(Node* node) {
        if (!node) return;
        if (node->prev) node->prev->next = node->next;
        else head_ = node->next;
        if (node->next) node->next->prev = node->prev;
        else tail_ = node->prev;
        delete node;
        size_--;
    }

    // Iterator
    class Iterator {
    public:
        Iterator(Node* node) : node_(node) {}
        T& operator*() { return node_->data; }
        T* operator->() { return &node_->data; }
        Iterator& operator++() { node_ = node_->next; return *this; }
        Iterator operator++(int) { Iterator tmp = *this; node_ = node_->next; return tmp; }
        bool operator==(const Iterator& other) const { return node_ == other.node_; }
        bool operator!=(const Iterator& other) const { return node_ != other.node_; }
    private:
        Node* node_;
    };

    Iterator begin() { return Iterator(head_); }
    Iterator end() { return Iterator(nullptr); }

    // Reverse iterator (from tail → head)
    class RIterator {
    public:
        RIterator(Node* node) : node_(node) {}
        T& operator*() { return node_->data; }
        T* operator->() { return &node_->data; }
        RIterator& operator++() { node_ = node_->prev; return *this; }
        RIterator operator++(int) { RIterator tmp = *this; node_ = node_->prev; return tmp; }
        bool operator==(const RIterator& other) const { return node_ == other.node_; }
        bool operator!=(const RIterator& other) const { return node_ != other.node_; }
    private:
        Node* node_;
    };

    RIterator rbegin() { return RIterator(tail_); }
    RIterator rend() { return RIterator(nullptr); }

    Node* head_ptr() { return head_; }
    Node* tail_ptr() { return tail_; }

private:
    Node* head_;
    Node* tail_;
    size_type size_;
};

} // namespace minidb
