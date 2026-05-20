/**
 * @file page_server.h
 * @brief Shared-storage PageServer MVP used by RemotePageStore.
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "container/hash_map.h"
#include "container/string.h"
#include "container/vector.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/page_store.h"
#include <array>
#include <atomic>

namespace minidb {

struct PageLogIndexEntry {
    LSN lsn;
    u64 wal_offset;
    PageLogIndexEntry() : lsn(0), wal_offset(0) {}
    PageLogIndexEntry(LSN l, u64 o) : lsn(l), wal_offset(o) {}
};

struct PageMetadata {
    LSN latest_lsn;
    u64 latest_wal_offset;
    PageMetadata() : latest_lsn(0), latest_wal_offset(0) {}
};

struct PageVersion {
    LSN lsn;
    u64 wal_offset;
    std::array<byte, kPageSize> data;
    PageVersion() : lsn(0), wal_offset(0), data{} {}
};

struct PageServerStats {
    u64 read_ops = 0;
    u64 write_ops = 0;
    u64 batch_read_ops = 0;
    u64 batch_write_ops = 0;
    u64 wal_image_bytes = 0;
    u64 lazy_apply_hits = 0;
    u64 future_page_fallbacks = 0;
    u64 rejected_writes = 0;
};

struct PageServerShard {
    mutable Mutex latch;
    HashMap<PageId, Vector<PageVersion>> versions;
    HashMap<PageId, Vector<PageLogIndexEntry>> log_index;
    HashMap<PageId, PageMetadata> page_metadata;
};

class PageServer : NonCopyable {
public:
    PageServer(const String& storage_dir, bool doublewrite_enabled,
               bool page_checksum_enabled, u32 fd_cache_limit, u32 replica_count = 0,
               u32 cached_versions_per_page = 32);
    ~PageServer();

    void read_page(PageId page_id, byte* page_data);
    bool read_page(PageId page_id, LSN read_lsn, byte* page_data);
    bool write_page(PageId page_id, const byte* page_data, LSN page_lsn);
    Vector<PageIOResult> read_pages(const Vector<PageReadRequest>& pages);
    Vector<PageIOResult> write_pages(const Vector<PageWriteRequest>& pages);
    void flush();
    void delete_file(const String& filename);

    void set_durable_lsn(LSN durable_lsn);
    LSN durable_lsn() const { return durable_lsn_; }
    LSN latest_page_lsn(PageId page_id) const;
    u32 replica_count() const { return replicas_.size(); }
    u32 log_index_size(PageId page_id) const;
    u32 cached_version_count(PageId page_id) const;
    u32 cached_versions_per_page() const { return cached_versions_per_page_; }
    PageServerStats stats() const;

private:
    void load_wal_index();
    void save_metadata_locked();
    u64 append_wal_image_locked(PageId page_id, const byte* page_data, LSN page_lsn);
    bool read_wal_image(u64 offset, byte* page_data) const;
    PageServerShard& shard_for(PageId page_id);
    const PageServerShard& shard_for(PageId page_id) const;
    bool find_log_entry_locked(const PageServerShard& shard, PageId page_id,
                               LSN read_lsn, PageLogIndexEntry* out) const;
    void insert_log_entry_locked(PageServerShard& shard, PageId page_id,
                                 const PageLogIndexEntry& entry);
    void remember_version_locked(PageServerShard& shard, PageId page_id,
                                 const byte* page_data, LSN page_lsn, u64 wal_offset);

    String storage_dir_;
    String wal_image_path_;
    String metadata_path_;
    DiskManager primary_;
    Vector<DiskManager*> replicas_;
    mutable Mutex latch_;
    LSN durable_lsn_;
    u32 cached_versions_per_page_;
    u64 wal_image_bytes_;
    static constexpr u32 kShardCount = 64;
    std::array<PageServerShard, kShardCount> shards_;
    std::atomic<u64> read_ops_;
    std::atomic<u64> write_ops_;
    std::atomic<u64> batch_read_ops_;
    std::atomic<u64> batch_write_ops_;
    std::atomic<u64> lazy_apply_hits_;
    std::atomic<u64> future_page_fallbacks_;
    std::atomic<u64> rejected_writes_;
    std::atomic<u64> wal_image_bytes_stat_;
};

class RemotePageStore : public PageStore {
public:
    RemotePageStore(PageServer* server, bool read_only = false, LSN read_lsn = 0,
                    u32 batch_size = 64)
        : server_(server), read_only_(read_only), read_lsn_(read_lsn),
          batch_size_(batch_size == 0 ? 1 : batch_size) {}

    Result<void> read_page(PageId page_id, byte* page_data) override;
    Result<void> write_page(PageId page_id, const byte* page_data, LSN page_lsn) override;
    Result<void> flush() override;
    Result<void> delete_file(const String& filename) override;
    Vector<PageIOResult> read_pages(const Vector<PageReadRequest>& pages) override;
    Vector<PageIOResult> write_pages(const Vector<PageWriteRequest>& pages) override;
    void set_durable_lsn(LSN durable_lsn) override;
    LSN durable_lsn() const override;
    bool is_remote() const override { return true; }

private:
    PageServer* server_;
    bool read_only_;
    LSN read_lsn_;
    u32 batch_size_;
};

} // namespace minidb
