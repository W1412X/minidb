/**
 * @file index_iterator.h
 * @brief B+ Tree index scan iterator
 */
#pragma once

#include "common/defs.h"
#include "index/btree.h"
#include "storage/buffer_pool.h"

namespace minidb {

class IndexIterator {
public:
    IndexIterator(BPlusTree* tree, BufferPool* pool);

    void seek(const Value& key);
    void seek_range(const Value& low, const Value& high);

    bool has_next();
    RecordId next();

private:
    BPlusTree* tree_;
    Vector<RecordId> results_;
    u32 current_idx_;
    bool initialized_;
};

} // namespace minidb
