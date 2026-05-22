/**
 * @file disk_manager.cpp
 * @brief DiskManager implementation
 */
#include "storage/disk_manager.h"
#include "common/config.h"
#include "storage/page.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdlib>

namespace minidb {

static constexpr u64 kDoubleWriteMagic = 0x4D44425752495445ULL; // "MDBWRITE"
static constexpr u16 kChecksumSeed = 0xA5A5;

static u16 page_checksum(const byte* data) {
    u32 sum = kChecksumSeed;
    for (u32 i = 0; i < kPageSize; i++) {
        if (i == 22 || i == 23) continue; // PageHeader::reserved stores checksum.
        sum = (sum + data[i]) & 0xFFFFu;
        sum = ((sum << 1) | (sum >> 15)) & 0xFFFFu;
    }
    return static_cast<u16>(sum == 0 ? 1 : sum);
}

static bool page_has_checksum(const byte* data) {
    const PageHeader* hdr = reinterpret_cast<const PageHeader*>(data);
    return hdr->page_type != 0 && hdr->reserved != 0;
}

#pragma pack(push, 1)
struct DoubleWriteHeader {
    u64 magic;
    PageId page_id;
    u16 checksum;
    u16 reserved;
    u32 page_size;
};
#pragma pack(pop)

// ============================================================
// Constructor / Destructor
// ============================================================

DiskManager::DiskManager(const String& db_path)
    : DiskManager(db_path, true, true, 1024) {}

DiskManager::DiskManager(const String& db_path, bool doublewrite_enabled,
                         bool page_checksum_enabled, u32 fd_cache_limit)
    : db_path_(db_path), doublewrite_path_(db_path + "/doublewrite.bin"), next_file_id_(1),
      doublewrite_enabled_(doublewrite_enabled), page_checksum_enabled_(page_checksum_enabled),
      fd_cache_limit_(fd_cache_limit == 0 ? 1 : fd_cache_limit) {
    // Create the database root directory.
    mkdir(db_path_.c_str(), 0755);

    // Create the standard subdirectories.
    String catalog_dir = db_path_ + "/catalog";
    String tables_dir  = db_path_ + "/tables";
    String indexes_dir = db_path_ + "/indexes";
    String wal_dir     = db_path_ + "/wal";
    mkdir(catalog_dir.c_str(), 0755);
    mkdir(tables_dir.c_str(), 0755);
    mkdir(indexes_dir.c_str(), 0755);
    mkdir(wal_dir.c_str(), 0755);
    if (doublewrite_enabled_) recover_double_write();
}

DiskManager::~DiskManager() {
    flush();
    // Close all file descriptors
    LockGuard guard(latch_);
    for (auto it = fd_cache_.begin(); it; it = fd_cache_.next(it)) {
        close(it->value);
    }
}

// ============================================================
// Page read/write
// ============================================================

void DiskManager::read_page(PageId pid, byte* page_data) {
    if (!read_page_direct(pid, page_data)) return;
    if (page_checksum_enabled_ && page_has_checksum(page_data)) {
        const PageHeader* hdr = reinterpret_cast<const PageHeader*>(page_data);
        if (hdr->reserved != page_checksum(page_data)) {
            // Torn/corrupt page: zero the buffer so callers never see half-written
            std::memset(page_data, 0, kPageSize);
        }
    }
}

void DiskManager::write_page(PageId pid, const byte* page_data) {
    byte page_copy[kPageSize];
    std::memcpy(page_copy, page_data, kPageSize);
    PageHeader* hdr = reinterpret_cast<PageHeader*>(page_copy);
    if (page_checksum_enabled_ && hdr->page_type != 0) {
        hdr->reserved = 0;
        hdr->reserved = page_checksum(page_copy);
    }

    int dw_fd = -1;
    if (doublewrite_enabled_) dw_fd = open(doublewrite_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (doublewrite_enabled_ && dw_fd >= 0) {
        DoubleWriteHeader dw;
        dw.magic = kDoubleWriteMagic;
        dw.page_id = pid;
        dw.checksum = page_checksum(page_copy);
        dw.reserved = 0;
        dw.page_size = kPageSize;
        if (pwrite(dw_fd, &dw, sizeof(dw), 0) == static_cast<ssize_t>(sizeof(dw))) {
            (void)pwrite(dw_fd, page_copy, kPageSize, sizeof(dw));
            fsync(dw_fd);
        }
        close(dw_fd);
    }

    if (!write_page_direct(pid, page_copy, false)) return;

    if (doublewrite_enabled_) dw_fd = open(doublewrite_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (doublewrite_enabled_ && dw_fd >= 0) {
        DoubleWriteHeader clear;
        std::memset(&clear, 0, sizeof(clear));
        if (pwrite(dw_fd, &clear, sizeof(clear), 0) == static_cast<ssize_t>(sizeof(clear))) {
            fsync(dw_fd);
        }
        close(dw_fd);
    }
}

// ============================================================
// File management
// ============================================================

u32 DiskManager::create_file(const String& filename) {
    LockGuard guard(latch_);
    u32 file_id = next_file_id_++;
    String path = db_path_ + "/" + filename;
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        fd_cache_[path] = fd;
    } else {
        file_id = 0;  // 0 indicates failure
    }
    return file_id;
}

void DiskManager::delete_file(const String& filename) {
    LockGuard guard(latch_);
    String path = db_path_ + "/" + filename;
    auto* fd = fd_cache_.find(path);
    if (fd) {
        close(*fd);
        fd_cache_.erase(path);
    }
    unlink(path.c_str());
}

u32 DiskManager::allocate_file_id() {
    LockGuard guard(latch_);
    return next_file_id_++;
}

void DiskManager::flush() {
    LockGuard guard(latch_);
    for (auto it = fd_cache_.begin(); it; it = fd_cache_.next(it)) {
        fsync(it->value);
    }
}

bool DiskManager::read_page_direct(PageId pid, byte* page_data) {
    String path = page_to_path(pid);
    u64 offset = page_to_offset(pid);
    int fd = get_fd(path);

    if (fd < 0) {
        std::memset(page_data, 0, kPageSize);
        return false;
    }
    ssize_t bytes = pread(fd, page_data, kPageSize, static_cast<off_t>(offset));
    if (bytes != kPageSize) {
        std::memset(page_data, 0, kPageSize);
        return false;
    }
    return true;
}

bool DiskManager::write_page_direct(PageId pid, const byte* page_data, bool fsync_after) {
    String path = page_to_path(pid);
    u64 offset = page_to_offset(pid);
    int fd = get_fd(path);

    if (fd < 0) return false;
    const byte* ptr = page_data;
    u32 remaining = kPageSize;
    off_t write_offset = static_cast<off_t>(offset);
    while (remaining > 0) {
        ssize_t written = pwrite(fd, ptr, remaining, write_offset);
        if (written <= 0) return false;
        ptr += written;
        write_offset += written;
        remaining -= static_cast<u32>(written);
    }
    if (fsync_after) fsync(fd);
    return true;
}

void DiskManager::recover_double_write() {
    int fd = open(doublewrite_path_.c_str(), O_RDONLY);
    if (fd < 0) return;
    DoubleWriteHeader dw;
    bool ok = false;
    if (pread(fd, &dw, sizeof(dw), 0) == static_cast<ssize_t>(sizeof(dw)) &&
        dw.magic == kDoubleWriteMagic && dw.page_size == kPageSize) {
        byte page[kPageSize];
        if (pread(fd, page, kPageSize, sizeof(dw)) == static_cast<ssize_t>(kPageSize) &&
            page_checksum(page) == dw.checksum) {
            ok = true;
            write_page_direct(dw.page_id, page, true);
        }
    }
    close(fd);
    if (ok) {
        int clear_fd = open(doublewrite_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (clear_fd >= 0) {
            fsync(clear_fd);
            close(clear_fd);
        }
    }
}

// ============================================================
// Internal methods
// ============================================================

String DiskManager::page_to_path(PageId pid) const {
    u32 file_id = file_id_from_page(pid);

    if (file_id == 1) {
        return db_path_ + "/catalog/1.cat";
    } else if (file_id < 1000) {
        // For now: file_id doubles as the table id.
        // Future: resolve the real table name through the catalog.
        return db_path_ + "/tables/" + String(static_cast<u32>(file_id)) + ".heap";
    } else {
        return db_path_ + "/indexes/" + String(static_cast<u32>(file_id)) + ".btree";
    }
}

u64 DiskManager::page_to_offset(PageId pid) const {
    u32 page_num = page_num_from_page(pid);
    return static_cast<u64>(page_num) * kPageSize;
}

int DiskManager::get_fd(const String& path) {
    LockGuard guard(latch_);
    auto* fd = fd_cache_.find(path);
    if (fd) return *fd;

    int new_fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (new_fd < 0) return -1;  // Don't cache failed fd
    if (fd_cache_.size() >= fd_cache_limit_) {
        for (auto it = fd_cache_.begin(); it; it = fd_cache_.next(it)) {
            close(it->value);
        }
        fd_cache_ = HashMap<String, int>();
    }
    fd_cache_[path] = new_fd;
    return new_fd;
}

} // namespace minidb
