/**
 * @file hash_map.h
 * @brief Custom Hash Map (open addressing + tombstones)
 */
#pragma once

#include "common/defs.h"
#include "container/utility.h"
#include "container/string.h"
#include <cstring>
#include <cstdlib>
#include <new>

namespace minidb {

using f64 = double;

// Hash function default implementation
template<typename K>
struct Hash {
    u64 operator()(const K& key) const {
        u64 h = 0;
        const byte* data = reinterpret_cast<const byte*>(&key);
        for (u64 i = 0; i < sizeof(K); i++) {
            h ^= data[i];
            h *= 0x100000001b3;
        }
        return h;
    }
};

template<>
struct Hash<u32> {
    u64 operator()(u32 key) const {
        key = ((key >> 16) ^ key) * 0x45d9f3b;
        key = ((key >> 16) ^ key) * 0x45d9f3b;
        key = (key >> 16) ^ key;
        return key;
    }
};

template<>
struct Hash<u64> {
    u64 operator()(u64 key) const {
        key = (~key) + (key << 21);
        key = key ^ (key >> 24);
        key = (key + (key << 3)) + (key << 8);
        key = key ^ (key >> 14);
        key = (key + (key << 2)) + (key << 4);
        key = key ^ (key >> 28);
        return key;
    }
};

template<>
struct Hash<i32> {
    u64 operator()(i32 key) const {
        return Hash<u32>()(static_cast<u32>(key));
    }
};

template<>
struct Hash<i64> {
    u64 operator()(i64 key) const {
        return Hash<u64>()(static_cast<u64>(key));
    }
};

template<>
struct Hash<String> {
    u64 operator()(const String& key) const {
        u64 h = 1469598103934665603ULL;
        const char* data = key.c_str();
        for (u32 i = 0; i < key.size(); i++) {
            h ^= static_cast<u8>(data[i]);
            h *= 1099511628211ULL;
        }
        return h;
    }
};

template<typename K, typename V>
class HashMap {
public:
    struct Entry {
        K key;
        V value;
        bool occupied;
        bool deleted;

        Entry() : key(), value(), occupied(false), deleted(false) {}
    };

    using size_type = u32;

    HashMap() : table_(nullptr), size_(0), capacity_(0), hash_func_() {}

    explicit HashMap(size_type initial_capacity)
        : size_(0), capacity_(initial_capacity), hash_func_() {
        table_ = static_cast<Entry*>(calloc(capacity_, sizeof(Entry)));
    }

    ~HashMap() {
        if (table_) {
            for (size_type i = 0; i < capacity_; i++) {
                if (table_[i].occupied) {
                    table_[i].key.~K();
                    table_[i].value.~V();
                }
            }
            free(table_);
        }
    }

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& other) noexcept
        : table_(other.table_), size_(other.size_),
          capacity_(other.capacity_), hash_func_(other.hash_func_) {
        other.table_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    HashMap& operator=(HashMap&& other) noexcept {
        if (this != &other) {
            if (table_) {
                for (size_type i = 0; i < capacity_; i++) {
                    if (table_[i].occupied) {
                        table_[i].key.~K();
                        table_[i].value.~V();
                    }
                }
                free(table_);
            }
            table_ = other.table_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.table_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    // Element access
    V& operator[](const K& key) {
        if (capacity_ == 0 || static_cast<f64>(size_) / capacity_ > 0.7) {
            rehash();
        }
        u64 h = hash_func_(key);
        size_type first_deleted = capacity_;
        for (size_type i = 0; i < capacity_; i++) {
            size_type idx = (h + i) % capacity_;
            if (table_[idx].occupied && !table_[idx].deleted && table_[idx].key == key) {
                return table_[idx].value;
            }
            if (!table_[idx].occupied && table_[idx].deleted) {
                if (first_deleted == capacity_) first_deleted = idx;
                continue;
            }
            if (!table_[idx].occupied) {
                size_type target = (first_deleted < capacity_) ? first_deleted : idx;
                new (&table_[target].key) K(key);
                new (&table_[target].value) V();
                table_[target].occupied = true;
                table_[target].deleted = false;
                size_++;
                return table_[target].value;
            }
        }
        if (first_deleted < capacity_) {
            new (&table_[first_deleted].key) K(key);
            new (&table_[first_deleted].value) V();
            table_[first_deleted].occupied = true;
            table_[first_deleted].deleted = false;
            size_++;
            return table_[first_deleted].value;
        }
        // All slots occupied after rehash — force another rehash
        rehash();
        // After rehash, capacity doubled, there must be free slots
        h = hash_func_(key);
        for (size_type i = 0; i < capacity_; i++) {
            size_type idx = (h + i) % capacity_;
            if (!table_[idx].occupied || table_[idx].deleted) {
                new (&table_[idx].key) K(key);
                new (&table_[idx].value) V();
                table_[idx].occupied = true;
                table_[idx].deleted = false;
                size_++;
                return table_[idx].value;
            }
        }
        // Should never reach here — abort to prevent data corruption
        std::abort();
    }

    V* find(const K& key) {
        if (capacity_ == 0) return nullptr;
        u64 h = hash_func_(key);
        for (size_type i = 0; i < capacity_; i++) {
            size_type idx = (h + i) % capacity_;
            if (!table_[idx].occupied && !table_[idx].deleted) return nullptr;
            if (table_[idx].occupied && !table_[idx].deleted && table_[idx].key == key) {
                return &table_[idx].value;
            }
        }
        return nullptr;
    }

    const V* find(const K& key) const {
        if (capacity_ == 0) return nullptr;
        u64 h = hash_func_(key);
        for (size_type i = 0; i < capacity_; i++) {
            size_type idx = (h + i) % capacity_;
            if (!table_[idx].occupied && !table_[idx].deleted) return nullptr;
            if (table_[idx].occupied && !table_[idx].deleted && table_[idx].key == key) {
                return &table_[idx].value;
            }
        }
        return nullptr;
    }

    bool contains(const K& key) const {
        return find(key) != nullptr;
    }

    // Modify
    void insert(const K& key, const V& value) {
        if (capacity_ == 0 || static_cast<f64>(size_) / capacity_ > 0.7) {
            rehash();
        }
        u64 h = hash_func_(key);
        size_type first_deleted = capacity_;
        for (size_type i = 0; i < capacity_; i++) {
            size_type idx = (h + i) % capacity_;
            if (table_[idx].occupied && !table_[idx].deleted && table_[idx].key == key) {
                table_[idx].value = value;
                return;
            }
            if (!table_[idx].occupied && table_[idx].deleted) {
                if (first_deleted == capacity_) first_deleted = idx;
                continue;
            }
            if (!table_[idx].occupied) {
                size_type target = (first_deleted < capacity_) ? first_deleted : idx;
                new (&table_[target].key) K(key);
                new (&table_[target].value) V(value);
                table_[target].occupied = true;
                table_[target].deleted = false;
                size_++;
                return;
            }
        }
        if (first_deleted < capacity_) {
            new (&table_[first_deleted].key) K(key);
            new (&table_[first_deleted].value) V(value);
            table_[first_deleted].occupied = true;
            table_[first_deleted].deleted = false;
            size_++;
            return;
        }
        rehash();
        insert(key, value);
    }

    bool erase(const K& key) {
        if (capacity_ == 0) return false;
        u64 h = hash_func_(key);
        for (size_type i = 0; i < capacity_; i++) {
            size_type idx = (h + i) % capacity_;
            if (!table_[idx].occupied && !table_[idx].deleted) return false;
            if (table_[idx].occupied && !table_[idx].deleted && table_[idx].key == key) {
                table_[idx].key.~K();
                table_[idx].value.~V();
                table_[idx].occupied = false;
                table_[idx].deleted = true;
                size_--;
                return true;
            }
        }
        return false;
    }

    void clear() {
        for (size_type i = 0; i < capacity_; i++) {
            if (table_[i].occupied) {
                table_[i].key.~K();
                table_[i].value.~V();
            }
            table_[i].occupied = false;
            table_[i].deleted = false;
        }
        size_ = 0;
    }

    size_type size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // Iteration (simplified: return Entry pointer)
    Entry* begin() {
        for (size_type i = 0; i < capacity_; i++) {
            if (table_[i].occupied) return &table_[i];
        }
        return nullptr;
    }

    Entry* end() { return nullptr; }

    Entry* next(Entry* current) {
        size_type idx = current - table_ + 1;
        for (size_type i = idx; i < capacity_; i++) {
            if (table_[i].occupied) return &table_[i];
        }
        return nullptr;
    }

private:
    void rehash() {
        size_type new_capacity = (capacity_ == 0) ? 16 : capacity_ * 2;
        Entry* new_table = static_cast<Entry*>(calloc(new_capacity, sizeof(Entry)));

        if (table_) {
            for (size_type i = 0; i < capacity_; i++) {
                if (table_[i].occupied) {
                    u64 h = hash_func_(table_[i].key);
                    for (size_type j = 0; j < new_capacity; j++) {
                        size_type idx = (h + j) % new_capacity;
                        if (!new_table[idx].occupied) {
                            new (&new_table[idx].key) K(move(table_[i].key));
                            new (&new_table[idx].value) V(move(table_[i].value));
                            new_table[idx].occupied = true;
                            new_table[idx].deleted = false;
                            table_[i].key.~K();
                            table_[i].value.~V();
                            break;
                        }
                    }
                }
            }
            free(table_);
        }

        table_ = new_table;
        capacity_ = new_capacity;
    }

    Entry* table_;
    size_type size_;
    size_type capacity_;
    Hash<K> hash_func_;
};

} // namespace minidb
