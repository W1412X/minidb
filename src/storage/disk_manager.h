/**
 * @file disk_manager.h
 * @brief Disk management — PageId to physical file mapping, low-level file I/O
 */
#pragma once

#include "common/defs.h"
#include "common/noncopyable.h"
#include "common/mutex.h"
#include "container/string.h"
#include "container/hash_map.h"

namespace minidb {

class DiskManager : NonCopyable {
public:
    explicit DiskManager(const String& db_path);
    DiskManager(const String& db_path, bool doublewrite_enabled,
                bool page_checksum_enabled, u32 fd_cache_limit);
    ~DiskManager();

    // Core: page read/write
    void read_page(PageId pid, byte* page_data);
    void write_page(PageId pid, const byte* page_data);

    // File management
    u32 create_file(const String& filename);
    void delete_file(const String& filename);
    u32 allocate_file_id();

    void flush();

    const String& db_path() const { return db_path_; }

private:
    void recover_double_write();
    bool write_page_direct(PageId pid, const byte* page_data, bool fsync_after);
    bool read_page_direct(PageId pid, byte* page_data);
    String page_to_path(PageId pid) const;
    u64 page_to_offset(PageId pid) const;
    int get_fd(const String& path);

    String db_path_;
    String doublewrite_path_;
    HashMap<String, int> fd_cache_;
    Mutex latch_;
    u32 next_file_id_;
    bool doublewrite_enabled_;
    bool page_checksum_enabled_;
    u32 fd_cache_limit_;
};

} // namespace minidb
