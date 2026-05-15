#include "sql/executor/index_scan_executor.h"
#include "transaction/transaction.h"

namespace minidb {

IndexScanExecutor::IndexScanExecutor(
    BufferPool* pool, HeapFile*, BPlusTree* index,
    const Value& search_key, bool is_range,
    const Value& range_high, const Schema& output_schema,
    TransactionManager* txn_mgr)
    : pool_(pool), index_(index),
      search_key_(search_key), is_range_(is_range),
      range_high_(range_high), output_schema_(output_schema),
      scan_leaf_id_(kNullPageId), scan_slot_idx_(0), last_index_rid_(),
      has_last_index_rid_(false), txn_mgr_(txn_mgr), last_rid_() {}

void IndexScanExecutor::init() {
    scan_leaf_id_ = kNullPageId;
    scan_slot_idx_ = 0;
    last_index_rid_ = RecordId();
    has_last_index_rid_ = false;
    last_rid_ = RecordId();
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

ExecResult IndexScanExecutor::next() {
    Value high = is_range_ ? range_high_ : search_key_;
    while (true) {
        RecordId rid;
        const RecordId* skip = has_last_index_rid_ ? &last_index_rid_ : nullptr;
        if (!index_->scan_next(search_key_, high, &scan_leaf_id_, &scan_slot_idx_, skip, &rid)) {
            break;
        }
        last_index_rid_ = rid;
        has_last_index_rid_ = true;
        auto result = pool_->fetch_page(rid.page_id);
        if (!result.ok()) continue;
        Page* page = result.value();
        const LinePointer* orig_lp = page->line_pointer(rid.slot_idx);
        SlotIdx visible_slot = rid.slot_idx;
        if (orig_lp && orig_lp->is_redirect()) {
            SlotIdx target = page->redirect_target(rid.slot_idx);
            if (target != kNullSlot) visible_slot = target;
        }
        const LinePointer* lp = page->line_pointer(visible_slot);
        if (lp && lp->is_valid()) {
            // Follow version chain to find visible version, return its real RecordId
            VersionResult vr = follow_version_chain(pool_, txn_mgr_, page, visible_slot,
                                                     output_schema_);
            pool_->unpin_page(rid.page_id);

            if (vr.tuple.column_count() > 0) {
                last_rid_ = vr.rid;  // Use visible version's RecordId, not the index's
                return ExecResult::ok(static_cast<Tuple&&>(vr.tuple));
            }
        } else {
            pool_->unpin_page(rid.page_id);
        }
    }
    return ExecResult::empty();
}

const Schema& IndexScanExecutor::output_schema() const { return output_schema_; }

bool IndexScanExecutor::last_record_id(RecordId* rid) const {
    if (!rid) return false;
    *rid = last_rid_;
    return last_rid_.page_id != kNullPageId;
}

IndexOnlyScanExecutor::IndexOnlyScanExecutor(BPlusTree* index, const Value& search_key,
                                             bool is_range, const Value& range_high,
                                             const Schema& output_schema)
    : index_(index), search_key_(search_key), is_range_(is_range),
      range_high_(range_high), output_schema_(output_schema),
      scan_leaf_id_(kNullPageId), scan_slot_idx_(0), last_index_rid_(),
      has_last_index_rid_(false) {}

void IndexOnlyScanExecutor::init() {
    scan_leaf_id_ = kNullPageId;
    scan_slot_idx_ = 0;
    last_index_rid_ = RecordId();
    has_last_index_rid_ = false;
}

ExecResult IndexOnlyScanExecutor::next() {
    Value high = is_range_ ? range_high_ : search_key_;
    while (true) {
        Value key;
        RecordId rid;
        const RecordId* skip = has_last_index_rid_ ? &last_index_rid_ : nullptr;
        if (!index_->scan_next_entry(search_key_, high, &scan_leaf_id_, &scan_slot_idx_,
                                     skip, &key, &rid)) {
            break;
        }
        last_index_rid_ = rid;
        has_last_index_rid_ = true;
        Vector<Value> values;
        values.push_back(key);
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
