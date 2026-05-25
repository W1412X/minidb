#include "sql/executor/index_scan_executor.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"
#include "transaction/transaction.h"
#include <cstring>

namespace minidb {

IndexScanExecutor::IndexScanExecutor(
    BufferPool* pool, HeapFile* heap, BPlusTree* index,
    const IndexKey& search_key, bool is_range,
    const IndexKey& range_high, const Schema& output_schema,
    TransactionManager* txn_mgr)
    : pool_(pool), index_(index),
      search_key_(search_key), is_range_(is_range),
      range_high_(range_high), output_schema_(output_schema),
      scan_leaf_id_(kNullPageId), scan_slot_idx_(0), last_index_rid_(),
      has_last_index_rid_(false), txn_mgr_(txn_mgr), last_rid_(),
      table_id_(heap ? heap->table_id() : 0), heap_(heap),
      cached_heap_page_id_(kNullPageId), cached_heap_page_(nullptr) {}

IndexScanExecutor::~IndexScanExecutor() {
    if (cached_heap_page_) {
        pool_->unpin_page(cached_heap_page_id_);
    }
}

void IndexScanExecutor::set_pushed_predicate(UniquePtr<Expression> pred) {
    pushed_predicate_ = static_cast<UniquePtr<Expression>&&>(pred);
    pushed_compile_ok_ = false;
}

void IndexScanExecutor::init() {
    scan_leaf_id_ = kNullPageId;
    scan_slot_idx_ = 0;
    last_index_rid_ = RecordId();
    has_last_index_rid_ = false;
    last_rid_ = RecordId();
    if (cached_heap_page_) {
        pool_->unpin_page(cached_heap_page_id_);
        cached_heap_page_ = nullptr;
        cached_heap_page_id_ = kNullPageId;
    }
    if (pushed_predicate_) {
        pushed_compile_ok_ = compiled_pushed_.compile(pushed_predicate_.get(),
                                                       output_schema_);
    } else {
        pushed_compile_ok_ = false;
    }
    batch_count_ = 0;
    batch_pos_ = 0;
    batch_exhausted_ = false;
}

// Traverse version chain to find first visible version, return its RecordId
struct VersionResult {
    Tuple tuple;
    RecordId rid;
};

static VersionResult follow_version_chain(BufferPool* pool, TransactionManager* txn_mgr,
                                           Page* start_page, u16 start_slot,
                                           const Schema& schema) {
    VersionResult result;
    const LinePointer* start_lp = start_page->line_pointer(start_slot);
    if (!start_lp || !start_lp->is_valid()) return result;
    result.tuple = Tuple::deserialize_from_page(
        start_page->data() + start_lp->offset, schema, start_lp->length);
    result.rid = RecordId(start_page->header()->page_id, start_slot);

    auto check_visible = [&](const Tuple& t) -> bool {
        if (!txn_mgr || !txn_mgr->current()) {
            return t.xmax() == kInvalidTxnId;
        }
        return txn_mgr->is_visible(t.xmin(), t.xmax(), *txn_mgr->current());
    };

    if (check_visible(result.tuple)) return result;

    u32 depth = 0;
    static constexpr u32 kMaxChainDepth = 64;

    // Traverse version chain old → new
    while (result.tuple.has_next_version() && depth < kMaxChainDepth) {
        PageId ver_page = result.tuple.next_version_page();
        SlotIdx ver_slot = result.tuple.next_version_slot();

        auto page_result = pool->fetch_page(ver_page, true);
        if (!page_result.ok()) {
            result.tuple = Tuple();
            return result;
        }

        Page* page = page_result.value();
        const LinePointer* lp = page->line_pointer(ver_slot);
        if (!lp || !lp->is_valid()) {
            pool->unpin_page(ver_page);
            result.tuple = Tuple();
            return result;
        }

        result.tuple = Tuple::deserialize_from_page(
            page->data() + lp->offset, schema, lp->length);
        result.rid = RecordId(ver_page, ver_slot);
        pool->unpin_page(ver_page);

        if (check_visible(result.tuple)) return result;
        depth++;
    }

    result.tuple = Tuple();
    return result;
}

static bool visible_header(BufferPool* pool, TransactionManager* txn_mgr,
                           Page* start_page, SlotIdx start_slot) {
    const LinePointer* lp = start_page->line_pointer(start_slot);
    if (!lp || !lp->is_valid() || lp->length < 26 ||
        static_cast<u32>(lp->offset) + 26 > kPageSize) {
        return false;
    }

    PageId page_id = start_page->header()->page_id;
    SlotIdx slot = start_slot;
    Page* page = start_page;
    bool pinned_current = false;
    u32 depth = 0;
    static constexpr u32 kMaxChainDepth = 64;

    while (depth < kMaxChainDepth) {
        lp = page->line_pointer(slot);
        if (!lp || !lp->is_valid() || lp->length < 26 ||
            static_cast<u32>(lp->offset) + 26 > kPageSize) {
            if (pinned_current) pool->unpin_page(page_id);
            return false;
        }

        u64 xmin = 0;
        u64 xmax = 0;
        PageId next_page = kNullPageId;
        SlotIdx next_slot = kNullSlot;
        const byte* base = page->data() + lp->offset;
        std::memcpy(&xmin, base, 8);
        std::memcpy(&xmax, base + 8, 8);
        std::memcpy(&next_page, base + 16, 8);
        std::memcpy(&next_slot, base + 24, 2);

        bool visible = false;
        if (!txn_mgr || !txn_mgr->current()) {
            visible = xmax == kInvalidTxnId;
        } else {
            visible = txn_mgr->is_visible(xmin, xmax, *txn_mgr->current());
        }
        if (visible) {
            if (pinned_current) pool->unpin_page(page_id);
            return true;
        }
        if (next_page == kNullPageId || next_slot == kNullSlot) {
            if (pinned_current) pool->unpin_page(page_id);
            return false;
        }

        if (pinned_current) pool->unpin_page(page_id);
        page_id = next_page;
        slot = next_slot;
        auto next = pool->fetch_page(page_id, true);
        if (!next.ok()) return false;
        page = next.value();
        pinned_current = true;
        depth++;
    }

    if (pinned_current) pool->unpin_page(page_id);
    return false;
}

ExecResult IndexScanExecutor::next() {
    IndexKey high = is_range_ ? range_high_ : search_key_;
    // Cache SSI tracking decision (avoid touching the txn_mgr per row).
    const bool track_reads = txn_mgr_ && txn_mgr_->current() &&
        txn_mgr_->current()->isolation() == IsolationLevel::kSerializable;
    while (true) {
        // Refill the batch buffer when the previous batch is drained. The
        // batched API pins the leaf page once per refill, so the work that
        // used to happen per-row (tree latch + partition latch + page hash
        // lookup + atomic pin/unpin pair) is amortised across kBatchSize
        // entries.
        if (batch_pos_ >= batch_count_) {
            if (batch_exhausted_) break;
            const RecordId* skip = has_last_index_rid_ ? &last_index_rid_ : nullptr;
            batch_count_ = index_->range_scan_batch(
                search_key_, high, &scan_leaf_id_, &scan_slot_idx_,
                skip, batch_keys_, batch_rids_, kBatchSize);
            batch_pos_ = 0;
            if (batch_count_ == 0) {
                batch_exhausted_ = true;
                break;
            }
            // Leaf id reset to kNullPageId by the iterator means we've
            // exhausted the index range; we still need to consume what
            // was returned but mustn't refill afterwards.
            if (scan_leaf_id_ == kNullPageId) batch_exhausted_ = true;
        }
        RecordId rid = batch_rids_[batch_pos_];
        batch_pos_++;
        last_index_rid_ = rid;
        has_last_index_rid_ = true;

        // Reuse the cached heap-page pin when the next RID lives on the same
        // page (typical for clustered indexes scanning sequentially-inserted
        // data). Drop the pin and refetch only when crossing pages.
        Page* page = nullptr;
        if (cached_heap_page_ && cached_heap_page_id_ == rid.page_id) {
            page = cached_heap_page_;
        } else {
            if (cached_heap_page_) {
                pool_->unpin_page(cached_heap_page_id_);
                cached_heap_page_ = nullptr;
                cached_heap_page_id_ = kNullPageId;
            }
            auto result = pool_->fetch_page(rid.page_id);
            if (!result.ok()) continue;
            cached_heap_page_ = result.value();
            cached_heap_page_id_ = rid.page_id;
            page = cached_heap_page_;
        }

        const LinePointer* orig_lp = page->line_pointer(rid.slot_idx);
        SlotIdx visible_slot = rid.slot_idx;
        if (orig_lp && orig_lp->is_redirect()) {
            SlotIdx target = page->redirect_target(rid.slot_idx);
            if (target != kNullSlot) visible_slot = target;
        }
        const LinePointer* lp = page->line_pointer(visible_slot);
        if (lp && lp->is_valid()) {
            // follow_version_chain may pin OTHER pages temporarily but always
            // unpins them before returning, so our cached pin survives.
            VersionResult vr = follow_version_chain(pool_, txn_mgr_, page, visible_slot,
                                                     output_schema_);
            if (vr.tuple.column_count() > 0) {
                // Apply pushed-down residual predicate inline. Rejected rows
                // never cross the operator boundary, saving a result move.
                if (pushed_predicate_) {
                    bool pass;
                    if (pushed_compile_ok_) {
                        pass = compiled_pushed_.passes(vr.tuple);
                    } else {
                        Value cond;
                        if (!ExpressionEvaluator::fast_evaluate(*pushed_predicate_,
                                                                 vr.tuple, &cond)) {
                            cond = ExpressionEvaluator::evaluate(*pushed_predicate_,
                                                                  vr.tuple);
                        }
                        pass = !cond.is_null() && cond.get_bool();
                    }
                    if (!pass) continue;
                }
                last_rid_ = vr.rid;
                if (track_reads) {
                    txn_mgr_->current()->record_read(table_id_, last_rid_);
                }
                return ExecResult::ok(static_cast<Tuple&&>(vr.tuple));
            }
            // Not visible — keep the page pinned for the next iteration; the
            // next index entry might still be on this page.
        }
    }
    if (cached_heap_page_) {
        pool_->unpin_page(cached_heap_page_id_);
        cached_heap_page_ = nullptr;
        cached_heap_page_id_ = kNullPageId;
    }
    return ExecResult::empty();
}

const Schema& IndexScanExecutor::output_schema() const { return output_schema_; }

bool IndexScanExecutor::fast_count(u64* count) {
    if (!count || pushed_predicate_) return false;
    *count = 0;

    IndexKey high = is_range_ ? range_high_ : search_key_;
    if (heap_) {
        u32 visible_pages = 0;
        u32 frozen_pages = 0;
        heap_->vm().stats(&visible_pages, &frozen_pages);
        (void)frozen_pages;
        if (heap_->meta().num_data_pages > 0 &&
            visible_pages >= heap_->meta().num_data_pages) {
            *count = index_->range_count(search_key_, high);
            return true;
        }
    }

    PageId leaf_id = kNullPageId;
    u16 slot_idx = 0;
    RecordId last_index_rid;
    bool has_last = false;
    IndexKey keys[kBatchSize];
    RecordId rids[kBatchSize];
    PageId cached_page_id = kNullPageId;
    Page* cached_page = nullptr;
    bool use_vm = heap_ && heap_->vm().size() != 0;
    PageId cached_vm_page_id = kNullPageId;
    bool cached_vm_visible = false;

    auto release_cached = [&]() {
        if (cached_page) {
            pool_->unpin_page(cached_page_id);
            cached_page = nullptr;
            cached_page_id = kNullPageId;
        }
    };

    while (true) {
        if (executor_cancelled()) {
            release_cached();
            return false;
        }
        const RecordId* skip = has_last ? &last_index_rid : nullptr;
        u32 n = index_->range_scan_batch(search_key_, high, &leaf_id, &slot_idx,
                                         skip, keys, rids, kBatchSize);
        if (n == 0) break;
        for (u32 i = 0; i < n; i++) {
            RecordId rid = rids[i];
            last_index_rid = rid;
            has_last = true;

            if (use_vm) {
                if (cached_vm_page_id != rid.page_id) {
                    cached_vm_page_id = rid.page_id;
                    cached_vm_visible = heap_->vm().is_visible(rid.page_id);
                }
                if (cached_vm_visible) {
                    (*count)++;
                    continue;
                }
            }

            Page* page = nullptr;
            if (cached_page && cached_page_id == rid.page_id) {
                page = cached_page;
            } else {
                release_cached();
                auto page_result = pool_->fetch_page(rid.page_id, true);
                if (!page_result.ok()) continue;
                cached_page = page_result.value();
                cached_page_id = rid.page_id;
                page = cached_page;
            }

            SlotIdx visible_slot = rid.slot_idx;
            const LinePointer* lp = page->line_pointer(visible_slot);
            if (lp && lp->is_redirect()) {
                SlotIdx target = page->redirect_target(visible_slot);
                if (target != kNullSlot) visible_slot = target;
                lp = page->line_pointer(visible_slot);
            }
            if (lp && lp->is_valid() && visible_header(pool_, txn_mgr_, page, visible_slot)) {
                (*count)++;
            }
        }
        if (leaf_id == kNullPageId) break;
    }

    release_cached();
    return true;
}

bool IndexScanExecutor::last_record_id(RecordId* rid) const {
    if (!rid) return false;
    *rid = last_rid_;
    return last_rid_.page_id != kNullPageId;
}

IndexOnlyScanExecutor::IndexOnlyScanExecutor(BufferPool* pool, BPlusTree* index,
                                             const IndexKey& search_key,
                                             bool is_range, const IndexKey& range_high,
                                             const Schema& output_schema,
                                             TransactionManager* txn_mgr,
                                             HeapFile* heap)
    : pool_(pool), index_(index), search_key_(search_key), is_range_(is_range),
      range_high_(range_high), output_schema_(output_schema),
      scan_leaf_id_(kNullPageId), scan_slot_idx_(0), last_index_rid_(),
      has_last_index_rid_(false), txn_mgr_(txn_mgr), heap_(heap) {}

void IndexOnlyScanExecutor::init() {
    scan_leaf_id_ = kNullPageId;
    scan_slot_idx_ = 0;
    last_index_rid_ = RecordId();
    has_last_index_rid_ = false;
    batch_count_ = 0;
    batch_pos_ = 0;
    batch_exhausted_ = false;
}

ExecResult IndexOnlyScanExecutor::next() {
    IndexKey high = is_range_ ? range_high_ : search_key_;
    while (true) {
        if (batch_pos_ >= batch_count_) {
            if (batch_exhausted_) break;
            const RecordId* skip = has_last_index_rid_ ? &last_index_rid_ : nullptr;
            batch_count_ = index_->range_scan_batch(
                search_key_, high, &scan_leaf_id_, &scan_slot_idx_,
                skip, batch_keys_, batch_rids_, kBatchSize);
            batch_pos_ = 0;
            if (batch_count_ == 0) {
                batch_exhausted_ = true;
                break;
            }
            if (scan_leaf_id_ == kNullPageId) batch_exhausted_ = true;
        }
        IndexKey key = static_cast<IndexKey&&>(batch_keys_[batch_pos_]);
        RecordId rid = batch_rids_[batch_pos_];
        batch_pos_++;
        last_index_rid_ = rid;
        has_last_index_rid_ = true;
        if (pool_) {
            // VM optimisation: if the Visibility Map marks this page as
            // all-visible, every live tuple on it is visible to every active
            // snapshot — skip the heap page fetch entirely.
            bool vm_visible = heap_ && heap_->vm().is_visible(rid.page_id);
            if (!vm_visible) {
                // Page not known to be all-visible — fall back to the
                // per-tuple header check on the heap page.
                auto page_result = pool_->fetch_page(rid.page_id, true);
                if (!page_result.ok()) continue;
                Page* page = page_result.value();
                SlotIdx visible_slot = rid.slot_idx;
                const LinePointer* lp = page->line_pointer(visible_slot);
                if (lp && lp->is_redirect()) {
                    SlotIdx target = page->redirect_target(visible_slot);
                    if (target != kNullSlot) visible_slot = target;
                    lp = page->line_pointer(visible_slot);
                }
                bool visible = false;
                if (lp && lp->is_valid()) {
                    visible = visible_header(pool_, txn_mgr_, page, visible_slot);
                }
                pool_->unpin_page(rid.page_id);
                if (!visible) continue;
            }
        }
        Vector<Value> values;
        values.push_back(key.first_value());
        return ExecResult::ok(Tuple(output_schema_, values));
    }
    return ExecResult::empty();
}

const Schema& IndexOnlyScanExecutor::output_schema() const { return output_schema_; }

bool IndexOnlyScanExecutor::last_record_id(RecordId* rid) const {
    if (!rid) return false;
    *rid = last_index_rid_;
    return has_last_index_rid_;
}

} // namespace minidb
