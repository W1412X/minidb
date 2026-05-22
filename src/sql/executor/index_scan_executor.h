/**
 * @file index_scan_executor.h / .cpp
 * @brief Index scan executor — uses B+ Tree to accelerate equality/range queries
 */
#pragma once

#include "sql/executor/executor.h"
#include "sql/executor/compiled_predicate.h"
#include "container/unique_ptr.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "index/btree.h"
#include "container/vector.h"

namespace minidb {

class TransactionManager;
struct Expression;

class IndexScanExecutor : public Executor {
public:
    IndexScanExecutor(BufferPool* pool, HeapFile* heap, BPlusTree* index,
                      const IndexKey& search_key, bool is_range,
                      const IndexKey& range_high, const Schema& output_schema,
                      TransactionManager* txn_mgr = nullptr);
    ~IndexScanExecutor() override;
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;
    bool last_record_id(RecordId* rid) const override;

    // Pushed-down residual predicate (the portion of WHERE that the index
    // range alone cannot satisfy). Evaluated inline so non-matching rows
    // never cross the operator boundary.
    void set_pushed_predicate(UniquePtr<Expression> pred);

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
    u32 table_id_;
    // Cache the last heap page pinned across next() calls. Clustered indexes
    // (the common case for INSERT-ordered data) return RIDs to the same heap
    // page in a run; reusing the pin saves a fetch/unpin pair per row.
    PageId cached_heap_page_id_;
    Page* cached_heap_page_;
    UniquePtr<Expression> pushed_predicate_;
    CompiledPredicate compiled_pushed_;
    bool pushed_compile_ok_ = false;

    // Batched range-scan buffer. range_scan_batch fills these with up to
    // kBatchSize entries per call, collapsing N×latch-and-pin work into a
    // single batch. The buffer is consumed by subsequent next() calls; when
    // empty we refill (or stop if the index iterator is exhausted).
    static constexpr u32 kBatchSize = 32;
    IndexKey batch_keys_[kBatchSize];
    RecordId batch_rids_[kBatchSize];
    u32 batch_count_ = 0;
    u32 batch_pos_ = 0;
    bool batch_exhausted_ = false;
};

class IndexOnlyScanExecutor : public Executor {
public:
    IndexOnlyScanExecutor(BufferPool* pool, BPlusTree* index, const IndexKey& search_key,
                          bool is_range, const IndexKey& range_high,
                          const Schema& output_schema, TransactionManager* txn_mgr = nullptr,
                          HeapFile* heap = nullptr);
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
    HeapFile* heap_;  // VM-based optimization: skip heap fetch for all-visible pages
    // Same batched-iterator buffer pattern as IndexScanExecutor — see the
    // comment there for rationale.
    static constexpr u32 kBatchSize = 32;
    IndexKey batch_keys_[kBatchSize];
    RecordId batch_rids_[kBatchSize];
    u32 batch_count_ = 0;
    u32 batch_pos_ = 0;
    bool batch_exhausted_ = false;
};

} // namespace minidb
