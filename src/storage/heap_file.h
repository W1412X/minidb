/**
 * @file heap_file.h
 * @brief Heap file management — owns every data page for one logical table.
 *
 * Layout: Page 0 (Meta Page) + Page 1+ (Data Pages)
 * Data pages are linked into a single linked list via next_page_id.
 */
#pragma once

#include "common/defs.h"
#include "common/config.h"
#include "common/mutex.h"
#include "common/status.h"
#include "container/utility.h"
#include "storage/page.h"
#include "storage/buffer_pool.h"
#include "storage/fsm.h"
#include "storage/visibility_map.h"

namespace minidb {

// Meta page data (stored in page 0 data area)
#pragma pack(push, 1)
struct HeapMeta {
    u32  table_id;
    u32  reserved;
    u64  first_data_page_id;    // First data page
    u64  last_data_page_id;     // last data page id
    u32  num_data_pages;        // total data pages
    u64  num_tuples;            // live tuple count
    u64  num_deleted_tuples;    // tuples marked deleted
};
#pragma pack(pop)

class HeapFile : NonCopyable {
public:
    HeapFile(BufferPool* pool, u32 table_id);
    ~HeapFile() = default;

    // Initialise a new heap file (creates Page 0 meta page).
    void create();

    // Insert a tuple and return (page_id, slot_idx).
    Result<Pair<PageId, SlotIdx>> insert_tuple(const byte* data, u16 length, u64 lsn = 0);

    // WAL-first two-phase insert.
    //
    // InsertReservation is a move-only RAII guard.  prepare_insert()
    // acquires the heap latch and returns a reservation.  The caller
    // writes the WAL record, then calls commit() to materialise the
    // tuple.  If the reservation is destroyed without commit(), the
    // latch is automatically released — no dangling lock on error paths.
    class InsertReservation {
    public:
        InsertReservation() : heap_(nullptr), committed_(false) {}
        InsertReservation(HeapFile* heap, PageId page_id, bool is_new_page, SlotIdx predicted_slot)
            : heap_(heap), page_id_(page_id), is_new_page_(is_new_page),
              predicted_slot_(predicted_slot), committed_(false) {}
        ~InsertReservation() { release(); }

        // Move-only.
        InsertReservation(InsertReservation&& o) noexcept
            : heap_(o.heap_), page_id_(o.page_id_), is_new_page_(o.is_new_page_),
              predicted_slot_(o.predicted_slot_), committed_(o.committed_) { o.heap_ = nullptr; }
        InsertReservation& operator=(InsertReservation&& o) noexcept {
            if (this != &o) { release(); heap_ = o.heap_; page_id_ = o.page_id_;
                is_new_page_ = o.is_new_page_; predicted_slot_ = o.predicted_slot_;
                committed_ = o.committed_; o.heap_ = nullptr; }
            return *this;
        }
        InsertReservation(const InsertReservation&) = delete;
        InsertReservation& operator=(const InsertReservation&) = delete;

        PageId page_id() const { return page_id_; }
        bool is_new_page() const { return is_new_page_; }
        SlotIdx predicted_slot() const { return predicted_slot_; }

        // Materialise the tuple.  Returns the actual (page_id, slot).
        // On success the reservation is consumed (latch released).
        Result<Pair<PageId, SlotIdx>> commit(const byte* data, u16 length, u64 lsn);

    private:
        void release() { if (heap_ && !committed_) { heap_->latch_.unlock(); heap_ = nullptr; } }
        HeapFile* heap_;
        PageId page_id_{kNullPageId};
        bool is_new_page_{false};
        SlotIdx predicted_slot_{kNullSlot};
        bool committed_{false};
    };

    Result<InsertReservation> prepare_insert(u16 length);

    // HOT same-page insert reservation.
    class InPageReservation {
    public:
        InPageReservation() : heap_(nullptr), committed_(false) {}
        InPageReservation(HeapFile* heap, PageId page_id, SlotIdx predicted_slot)
            : heap_(heap), page_id_(page_id), predicted_slot_(predicted_slot), committed_(false) {}
        ~InPageReservation() { release(); }

        InPageReservation(InPageReservation&& o) noexcept
            : heap_(o.heap_), page_id_(o.page_id_),
              predicted_slot_(o.predicted_slot_), committed_(o.committed_) { o.heap_ = nullptr; }
        InPageReservation& operator=(InPageReservation&& o) noexcept {
            if (this != &o) { release(); heap_ = o.heap_; page_id_ = o.page_id_;
                predicted_slot_ = o.predicted_slot_; committed_ = o.committed_; o.heap_ = nullptr; }
            return *this;
        }
        InPageReservation(const InPageReservation&) = delete;
        InPageReservation& operator=(const InPageReservation&) = delete;

        PageId page_id() const { return page_id_; }
        SlotIdx predicted_slot() const { return predicted_slot_; }

        Result<Pair<PageId, SlotIdx>> commit(const byte* data, u16 length, u64 lsn);

    private:
        void release() { if (heap_ && !committed_) { heap_->latch_.unlock(); heap_ = nullptr; } }
        HeapFile* heap_;
        PageId page_id_{kNullPageId};
        SlotIdx predicted_slot_{kNullSlot};
        bool committed_{false};
    };

    Result<InPageReservation> prepare_insert_in_page(PageId page_id, u16 length);

    // WAL-first: commit the old-page side of an UPDATE (set_next_version + mark_deleted + set_lsn, atomic).
    bool commit_old_tuple(PageId page_id, SlotIdx slot_idx,
                          PageId next_page, SlotIdx next_slot,
                          u64 xmax, u64 lsn);

    // WAL-first: stamp the page LSN (fallback used only inside commit_insert).
    void set_page_lsn(PageId page_id, u64 lsn);

    // HOT: insert a tuple onto a specific page (same-page version chain); returns kNullSlot on failure.
    Result<Pair<PageId, SlotIdx>> insert_tuple_in_page(PageId page_id,
                                                       const byte* data, u16 length, u64 lsn = 0);


    // Rollback an INSERT (mark the slot UNUSED).
    bool rollback_insert(PageId page_id, SlotIdx slot_idx, u64 lsn = 0);

    // Rollback a DELETE or old-version invalidation: clear xmax/next_version, restore chain tail.
    bool rollback_delete(PageId page_id, SlotIdx slot_idx, u64 lsn = 0);

    // MVCC: soft-delete (only stamp xmax, no physical removal).
    bool mark_deleted(PageId page_id, SlotIdx slot_idx, u64 xmax, u64 lsn = 0);

    // MVCC: set xmin (on INSERT).
    bool set_xmin(PageId page_id, SlotIdx slot_idx, u64 xmin, u64 lsn = 0);

    // Freeze a tuple: replace xmin with kFrozenTxnId so visibility checks
    // short-circuit.  Should only be called on committed, visible tuples
    // whose xmin is older than the freeze horizon.
    bool freeze_tuple(PageId page_id, SlotIdx slot_idx, u64 lsn = 0);

    // MVCC: set the version-chain pointer.
    bool set_next_version(PageId page_id, SlotIdx slot_idx, PageId next_page, SlotIdx next_slot,
                          u64 lsn = 0);

    // GC: flag a line pointer as DEAD.
    bool mark_dead(PageId page_id, SlotIdx slot_idx, u64 lsn = 0);
    bool prune_obsolete_version(PageId page_id, SlotIdx slot_idx, u64 oldest_active_txn,
                                u64 committed_xmax, u64 lsn = 0);
    /// @param out_new_physical_tuple if non-null, set to true when a new physical slot was actually written (used to decide whether the index needs a rebuild).
    bool recover_insert_at(PageId page_id, SlotIdx slot_idx, const byte* data, u16 length, u64 lsn,
                           bool* out_new_physical_tuple = nullptr);
    bool recover_update(PageId old_page_id, SlotIdx old_slot_idx,
                        PageId new_page_id, SlotIdx new_slot_idx,
                        u64 xmax, const byte* data, u16 length, u64 lsn);

    // Get the first data page id (entry point for full-table scans).
    PageId first_data_page_id() const;

    // Get metadata.
    const HeapMeta& meta() const { return meta_; }
    u32 table_id() const { return table_id_; }
    void flush_meta();

private:
    void ensure_meta_loaded() const;
    PageId meta_page_id() const;
    PageId allocate_new_page_id();
    void load_meta();
    void save_meta();
    void note_meta_changed();
    // Predict the next free slot on a page (no mutation, mirrors Page::insert_tuple).
    SlotIdx predict_slot(Page* page, u16 length) const;

    BufferPool* pool_;
    u32 table_id_;
    mutable HeapMeta meta_;
    mutable bool meta_loaded_;
    bool meta_dirty_;
    u32 meta_mutations_since_save_;
    mutable Mutex latch_;

    // P3: Free Space Map — tracks per-page free space for INSERT placement.
    FreeSpaceMap fsm_;

    // P3: Visibility Map — tracks all-visible / all-frozen pages.
    VisibilityMap vm_;

public:
    /// Access the Free Space Map for this heap.
    FreeSpaceMap& fsm() { return fsm_; }
    const FreeSpaceMap& fsm() const { return fsm_; }

    /// Access the Visibility Map for this heap.
    VisibilityMap& vm() { return vm_; }
    const VisibilityMap& vm() const { return vm_; }
};

} // namespace minidb
