/**
 * @file seq_scan.h
 * @brief Full table scan executor — MVCC visibility check + version chain traversal
 */
#pragma once

#include "sql/executor/executor.h"
#include "sql/executor/compiled_predicate.h"
#include "container/unique_ptr.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "index/btree.h"
#include "container/vector.h"
#include <vector>

namespace minidb {

class TransactionManager;
class Transaction;
struct Expression;

class SeqScanExecutor : public Executor {
public:
    SeqScanExecutor(BufferPool* pool, HeapFile* heap, const Schema& storage_schema,
                    const Schema& output_schema, const Vector<u32>& projected_columns,
                    TransactionManager* txn_mgr = nullptr);
    SeqScanExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                    TransactionManager* txn_mgr = nullptr);
    ~SeqScanExecutor() override;
    void init() override;
    ExecResult next() override;
    bool fast_count(u64* count) override;
    const Schema& output_schema() const override;

    // Install a pushed-down WHERE predicate. The scan owns the AST and
    // evaluates it inline on each visible tuple; rows failing the predicate
    // never leave the scan operator. Pass `nullptr` to clear.
    void set_pushed_predicate(UniquePtr<Expression> pred);

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
    // Hot-path optimization: pin the current heap page once, keep it pinned
    // across `next()` calls until we advance to the next page. The previous
    // code did fetch_page + unpin_page on every emitted tuple, which costs
    // two latch operations and a hash-table lookup per row.
    Page* pinned_page_;
    u16 pinned_num_tuples_;
    // Cached pre-scan of the current page: bit i is set iff slot i is the
    // TARGET of a REDIRECT line pointer somewhere on this page. Computing
    // it once per page replaces the previous O(N²) scan that iterated every
    // line pointer for every tuple emitted.
    static constexpr u32 kRedirectBitmapWords = 16;  // 1024 slots max
    u64 redirect_target_bits_[kRedirectBitmapWords];
    bool has_redirects_on_page_;
    // Tracks whether the SSI read tracking is needed at all so we avoid the
    // virtual function call cost for the common Snapshot-isolation case.
    bool track_reads_;

    void release_pinned_page();
    void prepare_page(Page* page);
    bool is_redirect_target_cached(SlotIdx slot) const;

    // Pushed-down WHERE predicate. When `pushed_predicate_` is non-null, the
    // scan evaluates it against every visible tuple and skips emission for
    // rows that fail. Keeping the filter inside the scan avoids the
    // per-row ExecResult move + virtual-call boundary that a separate
    // FilterExecutor would impose.
    UniquePtr<Expression> pushed_predicate_;
    CompiledPredicate compiled_pushed_;
    bool pushed_compile_ok_ = false;
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
