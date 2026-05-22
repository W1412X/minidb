/**
 * @file seq_scan.cpp
 * @brief MVCC full table scan — Visibility check + version chain traversal
 */
#include "sql/executor/seq_scan.h"
#include "sql/executor/filter.h"
#include "sql/executor/project.h"
#include "record/tuple.h"
#include "transaction/transaction.h"
#include <cstring>
#include <algorithm>
#include <thread>
#include <vector>

namespace minidb {

SeqScanExecutor::SeqScanExecutor(BufferPool* pool, HeapFile* heap, const Schema& storage_schema,
                                 const Schema& output_schema,
                                 const Vector<u32>& projected_columns,
                                 TransactionManager* txn_mgr)
    : pool_(pool), heap_(heap), storage_schema_(storage_schema), output_schema_(output_schema),
      projected_columns_(projected_columns),
      current_page_id_(kNullPageId), current_slot_(0), finished_(false),
      scan_file_id_(0), scan_next_page_num_(0), pages_remaining_(0),
      last_rid_(), txn_mgr_(txn_mgr) {}

SeqScanExecutor::SeqScanExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                                 TransactionManager* txn_mgr)
    : SeqScanExecutor(pool, heap, schema, schema, Vector<u32>(), txn_mgr) {}

void SeqScanExecutor::init() {
    PageId first = heap_->first_data_page_id();
    current_slot_ = 0;
    skipped_rids_.clear();
    if (first == kNullPageId) {
        current_page_id_ = kNullPageId;
        finished_ = true;
        scan_file_id_ = 0;
        scan_next_page_num_ = 0;
        pages_remaining_ = 0;
        return;
    }

    scan_file_id_ = file_id_from_page(first);
    scan_next_page_num_ = page_num_from_page(first);
    pages_remaining_ = heap_->meta().num_data_pages;
    current_page_id_ = make_page_id(scan_file_id_, scan_next_page_num_);
    finished_ = (pages_remaining_ == 0);
}

// Attempt MVCC visibility check on a single (page, slot)
ExecResult SeqScanExecutor::try_tuple(Page* page, u16 slot) {
    if (should_skip_rid(current_page_id_, slot)) {
        return ExecResult::empty();
    }

    const LinePointer* lp = page->line_pointer(slot);
    if (!lp) return ExecResult::empty();
    if (lp->is_redirect()) {
        SlotIdx target = page->redirect_target(slot);
        if (target == kNullSlot || should_skip_rid(current_page_id_, target)) {
            return ExecResult::empty();
        }
        const LinePointer* target_lp = page->line_pointer(target);
        if (!target_lp || !target_lp->is_valid()) return ExecResult::empty();
        last_rid_ = RecordId(current_page_id_, target);
        Tuple tuple = Tuple::deserialize_projected_from_page(
            page->data() + target_lp->offset, storage_schema_, output_schema_,
            projected_columns_, target_lp->length);
        if (!txn_mgr_ || !txn_mgr_->current()) {
            if (tuple.xmax() != kInvalidTxnId) return follow_latest_committed(page, target);
            return ExecResult::ok(static_cast<Tuple&&>(tuple));
        }
        if (txn_mgr_->is_visible(tuple.xmin(), tuple.xmax(), *txn_mgr_->current())) {
            if (txn_mgr_->current()) {
                txn_mgr_->current()->record_read(heap_->table_id(),
                                                 RecordId(current_page_id_, target));
            }
            return ExecResult::ok(static_cast<Tuple&&>(tuple));
        }
        return follow_version_chain(page, target);
    }
    if (!lp->is_valid()) return ExecResult::empty();
    if (is_redirect_target(page, slot)) return ExecResult::empty();

    Tuple tuple = Tuple::deserialize_projected_from_page(
        page->data() + lp->offset, storage_schema_, output_schema_,
        projected_columns_, lp->length);

    // No transaction manager → use autocommit visibility, hide deleted/expired versions.
    if (!txn_mgr_ || !txn_mgr_->current()) {
        if (tuple.xmax() != kInvalidTxnId) {
            return follow_latest_committed(page, slot);
        }
        last_rid_ = RecordId(current_page_id_, slot);
        return ExecResult::ok(static_cast<Tuple&&>(tuple));
    }

    // MVCC: Check if current version is visible to current transaction
    if (txn_mgr_->is_visible(tuple.xmin(), tuple.xmax(), *txn_mgr_->current())) {
        last_rid_ = RecordId(current_page_id_, slot);
        txn_mgr_->current()->record_read(heap_->table_id(), last_rid_);
        return ExecResult::ok(static_cast<Tuple&&>(tuple));
    }

    // Invisible → trace version chain
    return follow_version_chain(page, slot);
}

// Trace version chain to find first visible version
ExecResult SeqScanExecutor::follow_version_chain(Page* page, u16 slot) {
    const LinePointer* start_lp = page->line_pointer(slot);
    if (!start_lp || !start_lp->is_valid()) return ExecResult::empty();
    Tuple tuple = Tuple::deserialize_from_page(
        page->data() + start_lp->offset, storage_schema_, start_lp->length);

    u32 depth = 0;
    static constexpr u32 kMaxChainDepth = 64;  // Prevent infinite loop

    while (tuple.has_next_version() && depth < kMaxChainDepth) {
        PageId ver_page = tuple.next_version_page();
        SlotIdx ver_slot = tuple.next_version_slot();

        auto result = pool_->fetch_page(ver_page, true);
        if (!result.ok()) return ExecResult::empty();

        Page* prev_page = result.value();
        const LinePointer* prev_lp = prev_page->line_pointer(ver_slot);
        if (!prev_lp || !prev_lp->is_valid()) {
            pool_->unpin_page(ver_page);
            return ExecResult::empty();
        }

        tuple = Tuple::deserialize_from_page(
            prev_page->data() + prev_lp->offset, storage_schema_, prev_lp->length);

        if (txn_mgr_->is_visible(tuple.xmin(), tuple.xmax(), *txn_mgr_->current())) {
            mark_skip_rid(ver_page, ver_slot);
            last_rid_ = RecordId(ver_page, ver_slot);
            txn_mgr_->current()->record_read(heap_->table_id(), last_rid_);
            Tuple projected = Tuple::deserialize_projected_from_page(
                prev_page->data() + prev_lp->offset, storage_schema_, output_schema_,
                projected_columns_, prev_lp->length);
            pool_->unpin_page(ver_page);
            return ExecResult::ok(static_cast<Tuple&&>(projected));
        }
        pool_->unpin_page(ver_page);
        depth++;
    }

    return ExecResult::empty();
}

ExecResult SeqScanExecutor::follow_latest_committed(Page* page, u16 slot) {
    const LinePointer* start_lp = page->line_pointer(slot);
    if (!start_lp || !start_lp->is_valid()) return ExecResult::empty();
    Tuple tuple = Tuple::deserialize_from_page(
        page->data() + start_lp->offset, storage_schema_, start_lp->length);

    u32 depth = 0;
    static constexpr u32 kMaxChainDepth = 64;
    while (tuple.has_next_version() && depth < kMaxChainDepth) {
        PageId ver_page = tuple.next_version_page();
        SlotIdx ver_slot = tuple.next_version_slot();
        if (ver_page == kNullPageId || ver_slot == kNullSlot ||
            should_skip_rid(ver_page, ver_slot)) {
            return ExecResult::empty();
        }

        auto result = pool_->fetch_page(ver_page, true);
        if (!result.ok()) return ExecResult::empty();
        Page* version_page = result.value();
        const LinePointer* version_lp = version_page->line_pointer(ver_slot);
        if (!version_lp || !version_lp->is_valid()) {
            pool_->unpin_page(ver_page);
            return ExecResult::empty();
        }

        tuple = Tuple::deserialize_from_page(
            version_page->data() + version_lp->offset, storage_schema_, version_lp->length);
        if (tuple.xmax() == kInvalidTxnId) {
            mark_skip_rid(ver_page, ver_slot);
            last_rid_ = RecordId(ver_page, ver_slot);
            Tuple projected = Tuple::deserialize_projected_from_page(
                version_page->data() + version_lp->offset, storage_schema_, output_schema_,
                projected_columns_, version_lp->length);
            pool_->unpin_page(ver_page);
            return ExecResult::ok(static_cast<Tuple&&>(projected));
        }
        pool_->unpin_page(ver_page);
        depth++;
    }

    return ExecResult::empty();
}

bool SeqScanExecutor::is_redirect_target(const Page* page, SlotIdx slot_idx) const {
    if (!page) return false;
    u16 num_tuples = page->header()->num_tuples;
    for (u16 i = 0; i < num_tuples; i++) {
        const LinePointer* lp = page->line_pointer(i);
        if (lp && lp->is_redirect() && page->redirect_target(i) == slot_idx) {
            return true;
        }
    }
    return false;
}

bool SeqScanExecutor::should_skip_rid(PageId page_id, SlotIdx slot_idx) const {
    for (u32 i = 0; i < skipped_rids_.size(); i++) {
        const RecordId& rid = skipped_rids_[i];
        if (rid.page_id == page_id && rid.slot_idx == slot_idx) return true;
    }
    return false;
}

void SeqScanExecutor::mark_skip_rid(PageId page_id, SlotIdx slot_idx) {
    if (should_skip_rid(page_id, slot_idx)) return;
    skipped_rids_.push_back(RecordId(page_id, slot_idx));
}

ExecResult SeqScanExecutor::next() {
    while (!finished_) {
        if (pages_remaining_ == 0) {
            finished_ = true;
            break;
        }
        auto result = pool_->fetch_page(current_page_id_, true);
        if (!result.ok()) {
            pages_remaining_--;
            scan_next_page_num_++;
            if (pages_remaining_ == 0) {
                finished_ = true;
                current_page_id_ = kNullPageId;
            } else {
                current_page_id_ = make_page_id(scan_file_id_, scan_next_page_num_);
            }
            continue;
        }

        Page* page = result.value();
        u16 num_tuples = page->header()->num_tuples;

        while (current_slot_ < num_tuples) {
            u16 slot = current_slot_;
            current_slot_++;

            ExecResult tr = try_tuple(page, slot);
            if (tr.ok()) {
                pool_->unpin_page(current_page_id_);
                return tr;
            }
        }

        // Done with this page; advance to the next.
        pool_->unpin_page(current_page_id_);
        pages_remaining_--;
        current_slot_ = 0;
        scan_next_page_num_++;
        if (pages_remaining_ == 0) {
            current_page_id_ = kNullPageId;
            finished_ = true;
        } else {
            current_page_id_ = make_page_id(scan_file_id_, scan_next_page_num_);
        }
    }
    return ExecResult::empty();
}

const Schema& SeqScanExecutor::output_schema() const { return output_schema_; }

ParallelSeqScanExecutor::ParallelSeqScanExecutor(BufferPool* pool, HeapFile* heap,
                                                 const Schema& schema, u32 worker_count)
    : pool_(pool), heap_(heap), schema_(schema),
      worker_count_(worker_count == 0 ? 1 : worker_count), cursor_(0), last_rid_() {}

void ParallelSeqScanExecutor::init() {
    rows_.clear();
    cursor_ = 0;
    last_rid_ = RecordId();
    PageId first = heap_->first_data_page_id();
    u32 pages = heap_->meta().num_data_pages;
    if (first == kNullPageId || pages == 0) return;

    u32 file_id = file_id_from_page(first);
    u32 first_page_num = page_num_from_page(first);
    u32 workers = std::min(worker_count_, pages);
    if (workers <= 1) {
        workers = 1;
    }

    std::vector<std::vector<Tuple>> per_page(pages);
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (u32 w = 0; w < workers; w++) {
        threads.emplace_back([this, file_id, first_page_num, pages, workers, w, &per_page]() {
            for (u32 page_idx = w; page_idx < pages; page_idx += workers) {
                if (executor_cancelled()) return;
                PageId page_id = make_page_id(file_id, first_page_num + page_idx);
                auto result = pool_->fetch_page(page_id, true);
                if (!result.ok()) continue;
                Page* page = result.value();
                u16 num_tuples = page->header()->num_tuples;
                for (u16 slot = 0; slot < num_tuples; slot++) {
                    const LinePointer* lp = page->line_pointer(slot);
                    if (!lp || !lp->is_valid() || lp->is_redirect()) continue;
                    Tuple tuple = Tuple::deserialize_from_page(
                        page->data() + lp->offset, schema_, lp->length);
                    if (tuple.xmax() == kInvalidTxnId) {
                        per_page[page_idx].push_back(static_cast<Tuple&&>(tuple));
                    }
                }
                pool_->unpin_page(page_id);
            }
        });
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    u64 total = 0;
    for (const auto& page_rows : per_page) total += page_rows.size();
    rows_.reserve(static_cast<size_t>(total));
    for (auto& page_rows : per_page) {
        for (auto& row : page_rows) rows_.push_back(static_cast<Tuple&&>(row));
    }
}

ExecResult ParallelSeqScanExecutor::next() {
    if (cursor_ >= rows_.size()) return ExecResult::empty();
    last_rid_ = RecordId();
    Tuple tuple = static_cast<Tuple&&>(rows_[static_cast<size_t>(cursor_)]);
    cursor_++;
    return ExecResult::ok(static_cast<Tuple&&>(tuple));
}

const Schema& ParallelSeqScanExecutor::output_schema() const { return schema_; }

SeqScanExecutor* find_seq_scan(Executor* exec) {
    if (!exec) return nullptr;
    auto* scan = dynamic_cast<SeqScanExecutor*>(exec);
    if (scan) return scan;
    auto* f = dynamic_cast<FilterExecutor*>(exec);
    if (f) return find_seq_scan(f->child());
    auto* p = dynamic_cast<ProjectExecutor*>(exec);
    if (p) return find_seq_scan(p->child());
    return nullptr;
}

} // namespace minidb
