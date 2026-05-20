/**
 * @file index_scan_executor.h / .cpp
 * @brief Index scan executor — uses B+ Tree to accelerate equality/range queries
 */
#pragma once

#include "sql/executor/executor.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "index/btree.h"
#include "container/vector.h"

namespace minidb {

class TransactionManager;

class IndexScanExecutor : public Executor {
public:
    IndexScanExecutor(BufferPool* pool, HeapFile* heap, BPlusTree* index,
                      const IndexKey& search_key, bool is_range,
                      const IndexKey& range_high, const Schema& output_schema,
                      TransactionManager* txn_mgr = nullptr);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;
    bool last_record_id(RecordId* rid) const override;

private:
    BufferPool* pool_;
    BPlusTree* index_;
    IndexKey search_key_;
    bool is_range_;
    IndexKey range_high_;
    Schema output_schema_;
    PageId scan_leaf_id_;
    u16 scan_slot_idx_;
    RecordId last_index_rid_;
    bool has_last_index_rid_;
    TransactionManager* txn_mgr_;
    RecordId last_rid_;
};

class IndexOnlyScanExecutor : public Executor {
public:
    IndexOnlyScanExecutor(BufferPool* pool, BPlusTree* index, const IndexKey& search_key,
                          bool is_range, const IndexKey& range_high,
                          const Schema& output_schema, TransactionManager* txn_mgr = nullptr);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;
    bool last_record_id(RecordId* rid) const override;

private:
    BufferPool* pool_;
    BPlusTree* index_;
    IndexKey search_key_;
    bool is_range_;
    IndexKey range_high_;
    Schema output_schema_;
    PageId scan_leaf_id_;
    u16 scan_slot_idx_;
    RecordId last_index_rid_;
    bool has_last_index_rid_;
    TransactionManager* txn_mgr_;
};

} // namespace minidb
