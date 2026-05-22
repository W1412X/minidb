/**
 * @file gc.cpp
 * @brief MVCC Garbage Collection — Incremental: process max_pages pages, skip known clean pages
 */
#include "recovery/gc.h"
#include "database/database.h"
#include "transaction/transaction.h"
#include "catalog/catalog.h"
#include <cstring>

namespace minidb {

GarbageCollector::GarbageCollector(BufferPool* pool, TransactionManager* txn_mgr,
                                   Catalog* catalog, Database* db)
    : pool_(pool), txn_mgr_(txn_mgr), catalog_(catalog), db_(db) {}

bool GarbageCollector::is_garbage(const Tuple& t, u64 oldest_active) {
    if (t.xmin() == 0) return false;

    // xmin must be committed and < oldest_active
    if (!txn_mgr_->is_txn_committed(t.xmin())) return false;
    if (t.xmin() >= oldest_active) return false;

    // xmax == 0 → not deleted, not garbage
    if (t.xmax() == 0) return false;

    // xmax must be committed and < oldest_active
    if (!txn_mgr_->is_txn_committed(t.xmax())) return false;
    if (t.xmax() >= oldest_active) return false;

    return true;  // This version is invisible to all current and future transactions
}

void GarbageCollector::run_gc(u32 max_pages) {
    u64 oldest_active = txn_mgr_->get_oldest_active_txn_id();

    struct TableCtx {
        GarbageCollector* gc;
        u64 oldest;
        u32 pages_processed;
        u32 max_pages;
    };
    TableCtx ctx = {this, oldest_active, 0, max_pages};

    auto scan_callback = [](TableEntry& te, void* c) {
        auto* ctx = static_cast<TableCtx*>(c);
        if (ctx->pages_processed >= ctx->max_pages) return;

        GarbageCollector* gc = ctx->gc;
        HeapFile heap(gc->pool_, te.table_id);
        PageId first_page = heap.first_data_page_id();
        if (first_page == kNullPageId) return;
        u32 file_id = file_id_from_page(first_page);
        u32 pages = heap.meta().num_data_pages;
        if (pages == 0) return;

        // Incremental: resume from last processed page for this table
        u32* last = gc->last_gc_page_.find(te.table_id);
        u32 start_page = last ? *last : 0;
        if (start_page >= pages) start_page = 0;  // wrap around

        u32 processed = 0;
        for (u32 p = 0; p < pages && processed < ctx->max_pages; p++) {
            u32 page_idx = (start_page + p) % pages;
            PageId page_id = make_page_id(file_id, page_num_from_page(first_page) + page_idx);
            auto result = gc->pool_->fetch_page(page_id, true);
            if (!result.ok()) continue;

            Page* page = result.value();
            u16 num_tuples = page->header()->num_tuples;
            bool has_garbage = false;

            for (u16 slot = 0; slot < num_tuples; slot++) {
                const LinePointer* lp = page->line_pointer(slot);
                if (!lp || !lp->is_valid()) continue;

                Tuple tuple = Tuple::deserialize_from_page(
                    page->data() + lp->offset, te.schema, lp->length);

                if (gc->is_garbage(tuple, ctx->oldest)) {
                    has_garbage = true;
                    // Remove the index entries for this no-longer-visible tuple
                    // BEFORE we mark its slot DEAD. DELETE used to do this
                    // eagerly which broke SI visibility through IndexScan;
                    // now the index entry lives until GC.
                    if (gc->db_) {
                        gc->db_->delete_index_entries(te.table_id, tuple,
                            RecordId(page_id, slot));
                    }
                    if (tuple.has_next_version()) {
                        PageId next_page = tuple.next_version_page();
                        SlotIdx next_slot = tuple.next_version_slot();
                        if (next_page == page_id) {
                            // Same-page version chain: use REDIRECT to save space
                            if (page->redirect_slot(slot, next_slot)) {
                                gc->pool_->mark_dirty(page_id);
                            }
                        } else {
                            // Cross-page version chain: mark as DEAD
                            page->mark_dead(slot);
                            gc->pool_->mark_dirty(page_id);
                        }
                    } else {
                        // Chain tail: mark as DEAD
                        page->mark_dead(slot);
                        gc->pool_->mark_dirty(page_id);
                    }
                }
            }

            if (has_garbage) {
                // Prune DEAD tuples.
                page->prune();
                gc->pool_->mark_dirty(page_id);
            }

            gc->pool_->unpin_page(page_id);
            processed++;
            ctx->pages_processed++;
        }

        // Update resume point
        gc->last_gc_page_[te.table_id] = (start_page + processed) % pages;
    };

    catalog_->for_each_table(scan_callback, &ctx);
}

} // namespace minidb
