/**
 * @file seq_scan.cpp
 * @brief MVCC full table scan — Visibility check + version chain traversal
 */
#include "sql/executor/seq_scan.h"
#include "sql/executor/filter.h"
#include "sql/executor/project.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"
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
      last_rid_(), txn_mgr_(txn_mgr),
      pinned_page_(nullptr), pinned_num_tuples_(0),
      has_redirects_on_page_(false), track_reads_(false) {
    for (u32 i = 0; i < kRedirectBitmapWords; i++) redirect_target_bits_[i] = 0;
}

SeqScanExecutor::SeqScanExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                                 TransactionManager* txn_mgr)
    : SeqScanExecutor(pool, heap, schema, schema, Vector<u32>(), txn_mgr) {}

SeqScanExecutor::~SeqScanExecutor() {
    // Drop any page still pinned by an aborted iteration (LIMIT, error, etc.)
    release_pinned_page();
}

void SeqScanExecutor::release_pinned_page() {
    if (pinned_page_) {
        pool_->unpin_page(current_page_id_);
        pinned_page_ = nullptr;
        pinned_num_tuples_ = 0;
        has_redirects_on_page_ = false;
        for (u32 i = 0; i < kRedirectBitmapWords; i++) redirect_target_bits_[i] = 0;
    }
}

// Pre-scan the page once to find all REDIRECT targets; subsequent per-slot
// checks become O(1) bitmap lookups instead of O(num_tuples). Redirects are
// rare in INSERT-heavy workloads, so we also cache a fast-path flag.
void SeqScanExecutor::prepare_page(Page* page) {
    pinned_page_ = page;
    pinned_num_tuples_ = page->header()->num_tuples;
    has_redirects_on_page_ = false;
    for (u32 i = 0; i < kRedirectBitmapWords; i++) redirect_target_bits_[i] = 0;
    for (u16 i = 0; i < pinned_num_tuples_; i++) {
        const LinePointer* lp = page->line_pointer(i);
        if (lp && lp->is_redirect()) {
            SlotIdx target = page->redirect_target(i);
            if (target != kNullSlot && target < (kRedirectBitmapWords * 64)) {
                redirect_target_bits_[target >> 6] |= (1ULL << (target & 63));
                has_redirects_on_page_ = true;
            }
        }
    }
}

bool SeqScanExecutor::is_redirect_target_cached(SlotIdx slot) const {
    if (!has_redirects_on_page_) return false;
    if (slot >= (kRedirectBitmapWords * 64)) return false;
    return (redirect_target_bits_[slot >> 6] >> (slot & 63)) & 1ULL;
}

void SeqScanExecutor::set_pushed_predicate(UniquePtr<Expression> pred) {
    pushed_predicate_ = static_cast<UniquePtr<Expression>&&>(pred);
    pushed_compile_ok_ = false;   // re-compile on next init()
}

void SeqScanExecutor::init() {
    release_pinned_page();
    PageId first = heap_->first_data_page_id();
    current_slot_ = 0;
    skipped_rids_.clear();
    // SSI read tracking is only required for Serializable transactions; cache
    // the decision once at init() so the inner loop skips the per-row check.
    track_reads_ = txn_mgr_ && txn_mgr_->current() &&
                   txn_mgr_->current()->isolation() == IsolationLevel::kSerializable;
    // (Re-)compile pushed predicate against the output schema. We compile
    // against output_schema_ because the predicate has already been
    // rewritten by the planner to reference the columns this scan emits.
    if (pushed_predicate_) {
        pushed_compile_ok_ = compiled_pushed_.compile(pushed_predicate_.get(),
                                                       output_schema_);
    } else {
        pushed_compile_ok_ = false;
    }
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
    // O(1) bitmap check instead of O(num_tuples) line-pointer scan.
    if (is_redirect_target_cached(slot)) return ExecResult::empty();

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
        // Skip read tracking entirely for snapshot-isolation transactions
        // (the call is a no-op internally but still costs a function entry).
        if (track_reads_) {
            txn_mgr_->current()->record_read(heap_->table_id(), last_rid_);
        }
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
        // Pin the current page once; reuse it across many next() calls until
        // we exhaust its slots. The previous version did fetch+unpin per emit,
        // costing two latch ops and a hash lookup per row.
        if (!pinned_page_) {
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
            prepare_page(result.value());
            current_slot_ = 0;
        }

        // Emit tuples from the pinned page until we hit one that is visible
        // AND satisfies the pushed-down predicate (if any). Filter pushdown:
        // a row that fails the predicate never crosses this operator boundary.
        while (current_slot_ < pinned_num_tuples_) {
            u16 slot = current_slot_;
            current_slot_++;
            ExecResult tr = try_tuple(pinned_page_, slot);
            if (!tr.ok()) continue;

            if (pushed_predicate_) {
                bool pass;
                if (pushed_compile_ok_) {
                    pass = compiled_pushed_.passes(tr.tuple);
                } else {
                    Value cond;
                    if (!ExpressionEvaluator::fast_evaluate(*pushed_predicate_,
                                                             tr.tuple, &cond)) {
                        cond = ExpressionEvaluator::evaluate(*pushed_predicate_,
                                                              tr.tuple);
                    }
                    pass = !cond.is_null() && cond.get_bool();
                }
                if (!pass) continue;
            }
            // Page stays pinned — caller will come back for more tuples.
            return tr;
        }

        // Done with this page; advance to the next and drop the pin.
        release_pinned_page();
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
    release_pinned_page();
    return ExecResult::empty();
}

// fast_count — counts visible tuples without deserializing column data.
// AggregateExecutor delegates SELECT COUNT(*) to this when projection-free.
// We only read the per-tuple xmin/xmax header (16 bytes) and the line-pointer
// type bits, skipping Value/String allocation entirely. With our cached
// is_visible fast-path this is roughly an order of magnitude faster than the
// Volcano next() loop for COUNT(*) on large tables.
bool SeqScanExecutor::fast_count(u64* count) {
    if (!count) return false;
    // Fast count is only valid when the scan emits EVERY visible row.
    // A pushed-down predicate filters rows; falling back to the regular
    // next() loop keeps the count correct.
    if (pushed_predicate_) return false;
    *count = 0;
    PageId first = heap_->first_data_page_id();
    if (first == kNullPageId) return true;
    u32 file_id = file_id_from_page(first);
    u32 page_num = page_num_from_page(first);
    u32 pages = heap_->meta().num_data_pages;
    Transaction* txn = txn_mgr_ ? txn_mgr_->current() : nullptr;

    for (u32 p = 0; p < pages; p++, page_num++) {
        if (executor_cancelled()) return false;
        PageId pid = make_page_id(file_id, page_num);
        auto result = pool_->fetch_page(pid, true);
        if (!result.ok()) continue;
        Page* page = result.value();
        u16 num_tuples = page->header()->num_tuples;
        for (u16 slot = 0; slot < num_tuples; slot++) {
            const LinePointer* lp = page->line_pointer(slot);
            if (!lp || !lp->is_valid()) continue;
            if (lp->is_redirect()) continue;   // redirects are reached via target slot
            if (static_cast<u32>(lp->offset) + 16 > kPageSize) continue;
            // Read only xmin (8B) and xmax (8B); skip rest of the tuple.
            u64 xmin = 0, xmax = 0;
            const byte* base = page->data() + lp->offset;
            std::memcpy(&xmin, base, 8);
            std::memcpy(&xmax, base + 8, 8);
            bool visible;
            if (!txn) {
                visible = (xmax == kInvalidTxnId);
            } else {
                visible = txn_mgr_->is_visible(xmin, xmax, *txn);
            }
            if (visible) (*count)++;
        }
        pool_->unpin_page(pid);
    }
    return true;
}

struct FastAggState {
    AggFunc func;
    u64 count;
    Value value;
    bool has_value;
    double double_sum;

    FastAggState() : func(AggFunc::kCount), count(0), value(),
                     has_value(false), double_sum(0.0) {}
};

static bool fast_numeric_as_double(const Value& value, double* out) {
    if (!out || value.is_null()) return false;
    switch (value.type_id()) {
        case TypeId::kBool:
            *out = value.get_bool() ? 1.0 : 0.0;
            return true;
        case TypeId::kInt32:
            *out = static_cast<double>(value.get_int32());
            return true;
        case TypeId::kInt64:
            *out = static_cast<double>(value.get_int64());
            return true;
        case TypeId::kFloat:
            *out = static_cast<double>(value.get_float());
            return true;
        case TypeId::kDouble:
            *out = value.get_double();
            return true;
        default:
            return false;
    }
}

static Value finalize_fast_agg(const FastAggState& state) {
    if (!state.has_value) {
        if (state.func == AggFunc::kCount) return Value(static_cast<i64>(0));
        return Value();
    }
    if (state.func == AggFunc::kCount) return Value(static_cast<i64>(state.count));
    if (state.func == AggFunc::kAvg) {
        return state.count == 0 ? Value() : Value(state.double_sum / static_cast<double>(state.count));
    }
    return state.value;
}

bool SeqScanExecutor::fast_plain_aggregate(const Vector<AggregateColumn>& aggregates,
                                           Vector<Value>* row) {
    if (!row || aggregates.empty() || pushed_predicate_ || txn_mgr_) return false;

    Vector<int> column_indices;
    column_indices.reserve(aggregates.size());
    for (u32 a = 0; a < aggregates.size(); a++) {
        const AggregateColumn& agg = aggregates[a];
        if (agg.distinct) return false;
        if (!agg.argument) {
            if (agg.func != AggFunc::kCount) return false;
            column_indices.push_back(-1);
            continue;
        }
        const Expression* expr = agg.argument.get();
        if (!expr || expr->type != ExprType::kColumnRef) return false;
        int idx = expr->table_name.empty()
            ? storage_schema_.get_column_index(expr->column_name)
            : storage_schema_.get_column_index(expr->table_name, expr->column_name);
        if (idx < 0) return false;
        column_indices.push_back(idx);
    }

    Vector<FastAggState> states;
    states.resize(aggregates.size());
    for (u32 a = 0; a < aggregates.size(); a++) {
        states[a].func = aggregates[a].func;
    }

    PageId first = heap_->first_data_page_id();
    if (first == kNullPageId) {
        row->clear();
        for (u32 a = 0; a < states.size(); a++) row->push_back(finalize_fast_agg(states[a]));
        return true;
    }
    u32 file_id = file_id_from_page(first);
    u32 page_num = page_num_from_page(first);
    u32 pages = heap_->meta().num_data_pages;

    for (u32 p = 0; p < pages; p++, page_num++) {
        if (executor_cancelled()) return false;
        PageId pid = make_page_id(file_id, page_num);
        auto result = pool_->fetch_page(pid, true);
        if (!result.ok()) continue;
        Page* page = result.value();
        u16 num_tuples = page->header()->num_tuples;
        for (u16 slot = 0; slot < num_tuples; slot++) {
            const LinePointer* lp = page->line_pointer(slot);
            if (!lp || !lp->is_valid() || lp->is_redirect()) continue;
            if (static_cast<u32>(lp->offset) + 26 > kPageSize) continue;
            u64 xmax = 0;
            PageId next_page = kNullPageId;
            SlotIdx next_slot = kNullSlot;
            if (!Tuple::read_header_from_page(page->data() + lp->offset, lp->length,
                                              nullptr, &xmax, &next_page, &next_slot)) {
                pool_->unpin_page(pid);
                return false;
            }
            // Fall back for pages containing old versions; the regular scan
            // handles version-chain traversal. This keeps the fast path simple
            // and exact for all-visible append-mostly tables.
            (void)next_slot;
            if (xmax != kInvalidTxnId || next_page != kNullPageId) {
                pool_->unpin_page(pid);
                return false;
            }

            for (u32 a = 0; a < aggregates.size(); a++) {
                FastAggState& state = states[a];
                if (column_indices[a] < 0) {
                    state.count++;
                    state.has_value = true;
                    continue;
                }
                Value v;
                if (!Tuple::read_column_from_page(page->data() + lp->offset, storage_schema_,
                                                  static_cast<u32>(column_indices[a]),
                                                  lp->length, &v)) {
                    pool_->unpin_page(pid);
                    return false;
                }
                if (v.is_null()) continue;
                if (!state.has_value) {
                    state.value = v;
                    state.has_value = true;
                    state.count = 1;
                    double d = 0.0;
                    if (fast_numeric_as_double(v, &d)) state.double_sum = d;
                    continue;
                }
                switch (state.func) {
                    case AggFunc::kCount:
                        state.count++;
                        break;
                    case AggFunc::kSum:
                        state.value = state.value + v;
                        state.count++;
                        break;
                    case AggFunc::kAvg: {
                        double d = 0.0;
                        if (!fast_numeric_as_double(v, &d)) {
                            pool_->unpin_page(pid);
                            return false;
                        }
                        state.double_sum += d;
                        state.count++;
                        break;
                    }
                    case AggFunc::kMin:
                        if (v < state.value) state.value = v;
                        state.count++;
                        break;
                    case AggFunc::kMax:
                        if (v > state.value) state.value = v;
                        state.count++;
                        break;
                }
            }
        }
        pool_->unpin_page(pid);
    }

    row->clear();
    for (u32 a = 0; a < states.size(); a++) {
        row->push_back(finalize_fast_agg(states[a]));
    }
    return true;
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
