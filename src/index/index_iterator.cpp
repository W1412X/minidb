/**
 * @file index_iterator.cpp
 */
#include "index/index_iterator.h"

namespace minidb {

IndexIterator::IndexIterator(BPlusTree* tree, BufferPool*)
    : tree_(tree), current_idx_(0), initialized_(false) {}

void IndexIterator::seek(const Value& key) {
    results_ = tree_->search(key);
    current_idx_ = 0;
    initialized_ = true;
}

void IndexIterator::seek_range(const Value& low, const Value& high) {
    results_ = tree_->range_search(low, high);
    current_idx_ = 0;
    initialized_ = true;
}

bool IndexIterator::has_next() {
    if (!initialized_) return false;
    return current_idx_ < results_.size();
}

RecordId IndexIterator::next() {
    if (!has_next()) return RecordId();
    return results_[current_idx_++];
}

} // namespace minidb
