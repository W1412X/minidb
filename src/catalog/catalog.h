/**
 * @file catalog.h
 * @brief System catalog — manages table and index metadata
 */
#pragma once

#include "common/defs.h"
#include "common/config.h"
#include "container/string.h"
#include "container/hash_map.h"
#include "container/vector.h"
#include "record/schema.h"

namespace minidb {

struct ColumnStats {
    u64 ndv;           // Number of Distinct Values
    u64 null_count;    // NULL value count
    Value min_val;     // Minimum value (numeric types only)
    Value max_val;     // Maximum value (numeric types only)
    bool has_min_max;  // Whether min/max have been collected

    ColumnStats() : ndv(0), null_count(0), has_min_max(false) {}
};

struct TableEntry {
    u32      table_id;
    String   table_name;
    PageId   first_page_id;      // Heap file first page
    u32      num_pages;
    u64      num_tuples;
    Schema   schema;

    // Statistics (updated by ANALYZE)
    u64      stat_num_tuples;    // Estimated row count
    u32      stat_num_pages;     // estimated page count
    Vector<ColumnStats> col_stats;  // per-column statistics
    bool     stats_valid;        // whether statistics have been gathered

    TableEntry() : table_id(0), first_page_id(kNullPageId), num_pages(0),
                   num_tuples(0), stat_num_tuples(0), stat_num_pages(0),
                   stats_valid(false) {}
};

// Lifecycle state of an index. The optimiser only emits IndexScan /
// IndexOnlyScan / IndexLookupJoin plans when the entry is kValid. After
// crash recovery any heap-touching replay flips the entry to kInvalid
// until rebuild_index() finishes, at which point it goes back to kValid.
enum class IndexState : u8 {
    kValid     = 0,   // index file is in sync with heap; safe for the optimiser
    kInvalid   = 1,   // entries are missing or stale; rebuild needed before use
    kRebuilding = 2,  // a rebuild is in progress on this thread
};

struct IndexEntry {
    u32         index_id;
    String      index_name;
    u32         table_id;
    Vector<u32> key_columns;
    PageId      root_page_id;
    bool        is_unique;
    IndexState  state = IndexState::kValid;
};

class Catalog {
public:
    Catalog();
    ~Catalog() = default;

    // Table operations.
    u32 create_table(const String& name, const Schema& schema);
    TableEntry* get_table(const String& name);
    TableEntry* get_table(u32 table_id);
    bool drop_table(const String& name);
    void drop_indexes_for_table(u32 table_id);
    u32 table_count() const { return table_entries_.size(); }

    // Index operations.
    u32 create_index(const String& name, u32 table_id,
                     const Vector<u32>& key_columns, bool unique);
    IndexEntry* get_index(const String& name);
    IndexEntry* get_index(u32 index_id);
    Vector<IndexEntry*> get_indexes(u32 table_id);
    bool drop_index(const String& name);

    // Restore a previously dropped table/index entry (DDL undo).
    void restore_table(u32 table_id, const String& name, const Schema& schema);
    void restore_index(u32 index_id, const String& name, u32 table_id,
                       const Vector<u32>& key_columns, bool is_unique);

    // Query whether a specific column is indexed.
    bool is_column_indexed(u32 table_id, u32 col_idx) const;
    // Query whether any column in a group is indexed.
    bool any_column_indexed(u32 table_id, const Vector<u32>& col_indices) const;

    // Persistence.
    void save(const String& path);
    void load(const String& path);

    // Iterate
    void for_each_table(void (*callback)(TableEntry&, void*), void* ctx);
    void for_each_index(void (*callback)(IndexEntry&, void*), void* ctx);

private:
    HashMap<String, u32> name_to_table_id_;
    HashMap<u32, TableEntry> table_entries_;
    HashMap<String, u32> name_to_index_id_;
    HashMap<u32, IndexEntry> index_entries_;
    u32 next_table_id_;
    u32 next_index_id_;
};

} // namespace minidb
