/**
 * @file seq_scan.h
 * @brief Full table scan executor — MVCC visibility check + version chain traversal
 */
#pragma once

#include "sql/executor/executor.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "index/btree.h"
#include "container/vector.h"
#include <vector>

namespace minidb {

class TransactionManager;
class Transaction;

class SeqScanExecutor : public Executor {
public:
    SeqScanExecutor(BufferPool* pool, HeapFile* heap, const Schema& storage_schema,
                    const Schema& output_schema, const Vector<u32>& projected_columns,
                    TransactionManager* txn_mgr = nullptr);
    SeqScanExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                    TransactionManager* txn_mgr = nullptr);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

    RecordId last_record_id() const { return last_rid_; }
    bool last_record_id(RecordId* rid) const override {
        if (!rid) return false;
        *rid = last_rid_;
        return last_rid_.page_id != kNullPageId;
    }

private:
    ExecResult try_tuple(Page* page, u16 slot);
    ExecResult follow_version_chain(Page* page, u16 slot);
    ExecResult follow_latest_committed(Page* page, u16 slot);
    bool is_redirect_target(const Page* page, SlotIdx slot_idx) const;
    bool should_skip_rid(PageId page_id, SlotIdx slot_idx) const;
    void mark_skip_rid(PageId page_id, SlotIdx slot_idx);

    BufferPool* pool_;
    HeapFile* heap_;
    Schema storage_schema_;
    Schema output_schema_;
    Vector<u32> projected_columns_;
    PageId current_page_id_;
    u16 current_slot_;
    bool finished_;
    u32 scan_file_id_;
    u32 scan_next_page_num_;
    u32 pages_remaining_;
    RecordId last_rid_;
    TransactionManager* txn_mgr_;
    Vector<RecordId> skipped_rids_;
};

class ParallelSeqScanExecutor : public Executor {
public:
    ParallelSeqScanExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                            u32 worker_count);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;
    bool last_record_id(RecordId* rid) const override {
        if (!rid) return false;
        *rid = last_rid_;
        return last_rid_.page_id != kNullPageId;
    }

private:
    BufferPool* pool_;
    HeapFile* heap_;
    Schema schema_;
    u32 worker_count_;
    std::vector<Tuple> rows_;
    u64 cursor_;
    RecordId last_rid_;
};

SeqScanExecutor* find_seq_scan(Executor* exec);

} // namespace minidb
