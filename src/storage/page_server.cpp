#include "storage/page_server.h"
#include "storage/page.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace minidb {

static constexpr u64 kRemoteWalMagic = 0x4D44425057414C31ULL; // "MDBPWAL1"

#pragma pack(push, 1)
struct RemoteWalImageHeader {
    u64 magic;
    PageId page_id;
    LSN lsn;
    u32 page_size;
    u32 checksum;
};
#pragma pack(pop)

static LSN page_lsn_from_bytes(const byte* page_data) {
    if (!page_data) return 0;
    const PageHeader* hdr = reinterpret_cast<const PageHeader*>(page_data);
    return hdr->lsn;
}

static u32 image_checksum(const byte* data) {
    u32 sum = 0x9E3779B9u;
    for (u32 i = 0; i < kPageSize; i++) {
        sum ^= data[i];
        sum *= 16777619u;
    }
    return sum == 0 ? 1 : sum;
}

class WalImageBuffer {
public:
    WalImageBuffer() : buffer_(kPageSize, 0) {}

    byte* data() { return buffer_.data(); }
    const byte* data() const { return buffer_.data(); }

private:
    Vector<byte> buffer_;
};

static u32 upper_bound_lsn(const Vector<PageLogIndexEntry>& list, LSN lsn) {
    u32 left = 0;
    u32 right = list.size();
    while (left < right) {
        u32 mid = left + (right - left) / 2;
        if (list[mid].lsn <= lsn) left = mid + 1;
        else right = mid;
    }
    return left;
}

PageServer::PageServer(const String& storage_dir, bool doublewrite_enabled,
                       bool page_checksum_enabled, u32 fd_cache_limit,
                       u32 replica_count, u32 cached_versions_per_page)
    : storage_dir_(storage_dir),
      wal_image_path_(storage_dir + "/remote_wal_images.bin"),
      metadata_path_(storage_dir + "/page_server.meta"),
      primary_(storage_dir, doublewrite_enabled, page_checksum_enabled, fd_cache_limit),
      durable_lsn_(0),
      cached_versions_per_page_(cached_versions_per_page == 0 ? 1 : cached_versions_per_page),
      wal_image_bytes_(0),
      read_ops_(0),
      write_ops_(0),
      batch_read_ops_(0),
      batch_write_ops_(0),
      lazy_apply_hits_(0),
      future_page_fallbacks_(0),
      rejected_writes_(0),
      wal_image_bytes_stat_(0) {
    mkdir(storage_dir.c_str(), 0755);
    load_wal_index();
    for (u32 i = 0; i < replica_count; i++) {
        String replica_dir = storage_dir + "/replica_" + String(i + 1);
        mkdir(replica_dir.c_str(), 0755);
        replicas_.push_back(new DiskManager(replica_dir, doublewrite_enabled,
                                            page_checksum_enabled, fd_cache_limit));
    }
}

PageServer::~PageServer() {
    flush();
    for (u32 i = 0; i < replicas_.size(); i++) {
        delete replicas_[i];
    }
}

void PageServer::load_wal_index() {
    FILE* meta = std::fopen(metadata_path_.c_str(), "r");
    if (meta) {
        char key[64];
        unsigned long long value = 0;
        while (std::fscanf(meta, "%63[^=]=%llu\n", key, &value) == 2) {
            if (std::strcmp(key, "durable_lsn") == 0) {
                durable_lsn_ = static_cast<LSN>(value);
            }
        }
        std::fclose(meta);
    }

    FILE* wal = std::fopen(wal_image_path_.c_str(), "rb");
    if (!wal) return;
    WalImageBuffer wal_buf;
    while (true) {
        long offset = std::ftell(wal);
        if (offset < 0) break;
        RemoteWalImageHeader hdr;
        size_t n = std::fread(&hdr, 1, sizeof(hdr), wal);
        if (n == 0) break;
        if (n != sizeof(hdr) || hdr.magic != kRemoteWalMagic || hdr.page_size != kPageSize) break;
        if (std::fread(wal_buf.data(), 1, kPageSize, wal) != kPageSize) break;
        if (hdr.checksum != image_checksum(wal_buf.data())) break;

        PageServerShard& shard = shard_for(hdr.page_id);
        {
            LockGuard guard(shard.latch);
            insert_log_entry_locked(shard, hdr.page_id,
                                    PageLogIndexEntry(hdr.lsn, static_cast<u64>(offset)));
            PageMetadata& meta_entry = shard.page_metadata[hdr.page_id];
            if (hdr.lsn >= meta_entry.latest_lsn) {
                meta_entry.latest_lsn = hdr.lsn;
                meta_entry.latest_wal_offset = static_cast<u64>(offset);
            }
        }
        wal_image_bytes_ = static_cast<u64>(std::ftell(wal));
    }
    std::fclose(wal);
    wal_image_bytes_stat_.store(wal_image_bytes_, std::memory_order_relaxed);
}

void PageServer::save_metadata_locked() {
    String tmp = metadata_path_ + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "version=1\n");
    std::fprintf(f, "durable_lsn=%llu\n",
                 static_cast<unsigned long long>(durable_lsn_));
    std::fprintf(f, "wal_image_bytes=%llu\n",
                 static_cast<unsigned long long>(wal_image_bytes_));
    std::fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    std::fclose(f);
    std::rename(tmp.c_str(), metadata_path_.c_str());
}

u64 PageServer::append_wal_image_locked(PageId page_id, const byte* page_data, LSN page_lsn) {
    FILE* f = std::fopen(wal_image_path_.c_str(), "ab");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long offset = std::ftell(f);
    RemoteWalImageHeader hdr;
    hdr.magic = kRemoteWalMagic;
    hdr.page_id = page_id;
    hdr.lsn = page_lsn;
    hdr.page_size = kPageSize;
    hdr.checksum = image_checksum(page_data);
    bool ok = std::fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
              std::fwrite(page_data, 1, kPageSize, f) == kPageSize;
    std::fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    std::fclose(f);
    if (!ok || offset < 0) return 0;
    wal_image_bytes_ = static_cast<u64>(offset) + sizeof(hdr) + kPageSize;
    wal_image_bytes_stat_.store(wal_image_bytes_, std::memory_order_relaxed);
    return static_cast<u64>(offset);
}

bool PageServer::read_wal_image(u64 offset, byte* page_data) const {
    FILE* f = std::fopen(wal_image_path_.c_str(), "rb");
    if (!f) return false;
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    RemoteWalImageHeader hdr;
    bool ok = std::fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
              hdr.magic == kRemoteWalMagic && hdr.page_size == kPageSize &&
              std::fread(page_data, 1, kPageSize, f) == kPageSize &&
              hdr.checksum == image_checksum(page_data);
    std::fclose(f);
    return ok;
}

PageServerShard& PageServer::shard_for(PageId page_id) {
    return shards_[static_cast<u32>(page_id % kShardCount)];
}

const PageServerShard& PageServer::shard_for(PageId page_id) const {
    return shards_[static_cast<u32>(page_id % kShardCount)];
}

void PageServer::insert_log_entry_locked(PageServerShard& shard, PageId page_id,
                                         const PageLogIndexEntry& entry) {
    Vector<PageLogIndexEntry>& list = shard.log_index[page_id];
    if (list.empty() || list.back().lsn <= entry.lsn) {
        list.push_back(entry);
        return;
    }
    u32 pos = upper_bound_lsn(list, entry.lsn);
    list.resize(list.size() + 1);
    for (u32 i = list.size() - 1; i > pos; i--) {
        list[i] = list[i - 1];
    }
    list[pos] = entry;
}

bool PageServer::find_log_entry_locked(const PageServerShard& shard, PageId page_id,
                                       LSN read_lsn, PageLogIndexEntry* out) const {
    const Vector<PageLogIndexEntry>* list =
        const_cast<HashMap<PageId, Vector<PageLogIndexEntry>>&>(shard.log_index).find(page_id);
    if (!list || list->empty()) return false;

    u32 pos = upper_bound_lsn(*list, read_lsn);
    if (pos == 0) return false;
    if (out) *out = (*list)[pos - 1];
    return true;
}

void PageServer::remember_version_locked(PageServerShard& shard, PageId page_id,
                                         const byte* page_data,
                                         LSN page_lsn, u64 wal_offset) {
    PageVersion version;
    version.lsn = page_lsn;
    version.wal_offset = wal_offset;
    std::memcpy(version.data.data(), page_data, kPageSize);
    Vector<PageVersion>& page_versions = shard.versions[page_id];
    page_versions.push_back(version);
    while (page_versions.size() > cached_versions_per_page_) {
        page_versions.erase(page_versions.begin());
    }
    insert_log_entry_locked(shard, page_id, PageLogIndexEntry(page_lsn, wal_offset));
    PageMetadata& meta = shard.page_metadata[page_id];
    if (page_lsn >= meta.latest_lsn) {
        meta.latest_lsn = page_lsn;
        meta.latest_wal_offset = wal_offset;
    }
}

void PageServer::read_page(PageId page_id, byte* page_data) {
    read_ops_.fetch_add(1, std::memory_order_relaxed);
    primary_.read_page(page_id, page_data);
}

bool PageServer::read_page(PageId page_id, LSN read_lsn, byte* page_data) {
    read_ops_.fetch_add(1, std::memory_order_relaxed);
    primary_.read_page(page_id, page_data);
    if (read_lsn == 0) return true;

    LSN current = page_lsn_from_bytes(page_data);
    u64 wal_offset = 0;
    LSN wal_lsn = 0;
    bool need_wal = false;
    bool future_without_base = false;
    {
        const PageServerShard& shard = shard_for(page_id);
        LockGuard guard(shard.latch);
        PageLogIndexEntry entry;
        bool has_entry = find_log_entry_locked(shard, page_id, read_lsn, &entry);
        if (has_entry && entry.lsn > current) {
            wal_offset = entry.wal_offset;
            wal_lsn = entry.lsn;
            need_wal = true;
        } else if (current > read_lsn) {
            if (has_entry) {
                wal_offset = entry.wal_offset;
                wal_lsn = entry.lsn;
                need_wal = true;
            } else {
                future_without_base = true;
            }
            future_page_fallbacks_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (need_wal && read_wal_image(wal_offset, page_data)) {
        (void)wal_lsn;
        lazy_apply_hits_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (need_wal) return false;
    if (future_without_base) {
        return false;
    }
    return true;
}

bool PageServer::write_page(PageId page_id, const byte* page_data, LSN page_lsn) {
    if (!page_data) return false;
    LSN lsn = page_lsn == 0 ? page_lsn_from_bytes(page_data) : page_lsn;
    u64 wal_offset = 0;
    {
        LockGuard guard(latch_);
        if (durable_lsn_ != 0 && lsn > durable_lsn_) {
            rejected_writes_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        wal_offset = append_wal_image_locked(page_id, page_data, lsn);
        save_metadata_locked();
    }
    {
        PageServerShard& shard = shard_for(page_id);
        LockGuard guard(shard.latch);
        remember_version_locked(shard, page_id, page_data, lsn, wal_offset);
    }
    primary_.write_page(page_id, page_data);
    for (u32 i = 0; i < replicas_.size(); i++) {
        replicas_[i]->write_page(page_id, page_data);
    }
    write_ops_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

Vector<PageIOResult> PageServer::read_pages(const Vector<PageReadRequest>& pages) {
    Vector<PageIOResult> results;
    results.reserve(pages.size());
    batch_read_ops_.fetch_add(1, std::memory_order_relaxed);
    for (u32 i = 0; i < pages.size(); i++) {
        if (pages[i].data) {
            read_page(pages[i].page_id, pages[i].data);
            results.push_back(PageIOResult(pages[i].page_id, Status::ok_status()));
        } else {
            results.push_back(PageIOResult(pages[i].page_id,
                                           Status(ErrorCode::kInvalidArgument,
                                                  "null page buffer")));
        }
    }
    return results;
}

Vector<PageIOResult> PageServer::write_pages(const Vector<PageWriteRequest>& pages) {
    Vector<PageIOResult> results;
    results.reserve(pages.size());
    batch_write_ops_.fetch_add(1, std::memory_order_relaxed);
    for (u32 i = 0; i < pages.size(); i++) {
        if (pages[i].data) {
            bool ok = write_page(pages[i].page_id, pages[i].data, pages[i].page_lsn);
            results.push_back(PageIOResult(
                pages[i].page_id,
                ok ? Status::ok_status()
                   : Status(ErrorCode::kIOError, "PageServer rejected page write")));
        } else {
            results.push_back(PageIOResult(pages[i].page_id,
                                           Status(ErrorCode::kInvalidArgument,
                                                  "null page buffer")));
        }
    }
    return results;
}

void PageServer::flush() {
    {
        LockGuard guard(latch_);
        save_metadata_locked();
    }
    primary_.flush();
    for (u32 i = 0; i < replicas_.size(); i++) replicas_[i]->flush();
}

void PageServer::delete_file(const String& filename) {
    primary_.delete_file(filename);
    for (u32 i = 0; i < replicas_.size(); i++) replicas_[i]->delete_file(filename);
}

void PageServer::set_durable_lsn(LSN durable_lsn) {
    LockGuard guard(latch_);
    if (durable_lsn > durable_lsn_) {
        durable_lsn_ = durable_lsn;
        save_metadata_locked();
    }
}

LSN PageServer::latest_page_lsn(PageId page_id) const {
    const PageServerShard& shard = shard_for(page_id);
    LockGuard guard(shard.latch);
    const PageMetadata* meta =
        const_cast<HashMap<PageId, PageMetadata>&>(shard.page_metadata).find(page_id);
    return meta ? meta->latest_lsn : 0;
}

u32 PageServer::log_index_size(PageId page_id) const {
    const PageServerShard& shard = shard_for(page_id);
    LockGuard guard(shard.latch);
    const Vector<PageLogIndexEntry>* list =
        const_cast<HashMap<PageId, Vector<PageLogIndexEntry>>&>(shard.log_index).find(page_id);
    return list ? list->size() : 0;
}

u32 PageServer::cached_version_count(PageId page_id) const {
    const PageServerShard& shard = shard_for(page_id);
    LockGuard guard(shard.latch);
    const Vector<PageVersion>* list =
        const_cast<HashMap<PageId, Vector<PageVersion>>&>(shard.versions).find(page_id);
    return list ? list->size() : 0;
}

PageServerStats PageServer::stats() const {
    PageServerStats out;
    out.read_ops = read_ops_.load(std::memory_order_relaxed);
    out.write_ops = write_ops_.load(std::memory_order_relaxed);
    out.batch_read_ops = batch_read_ops_.load(std::memory_order_relaxed);
    out.batch_write_ops = batch_write_ops_.load(std::memory_order_relaxed);
    out.wal_image_bytes = wal_image_bytes_stat_.load(std::memory_order_relaxed);
    out.lazy_apply_hits = lazy_apply_hits_.load(std::memory_order_relaxed);
    out.future_page_fallbacks = future_page_fallbacks_.load(std::memory_order_relaxed);
    out.rejected_writes = rejected_writes_.load(std::memory_order_relaxed);
    return out;
}

Result<void> RemotePageStore::read_page(PageId page_id, byte* page_data) {
    if (!page_data) return Status(ErrorCode::kInvalidArgument, "null page buffer");
    if (read_only_ && read_lsn_ != 0) {
        if (!server_->read_page(page_id, read_lsn_, page_data)) {
            return Status(ErrorCode::kNotFound, "read_lsn cannot be satisfied");
        }
    } else {
        server_->read_page(page_id, page_data);
    }
    return Status::ok_status();
}

Result<void> RemotePageStore::write_page(PageId page_id, const byte* page_data, LSN page_lsn) {
    if (read_only_) {
        return Status(ErrorCode::kInvalidArgument, "read-only remote page store cannot write pages");
    }
    if (!server_->write_page(page_id, page_data, page_lsn)) {
        return Status(ErrorCode::kIOError, "PageServer rejected page write");
    }
    return Status::ok_status();
}

Result<void> RemotePageStore::flush() {
    server_->flush();
    return Status::ok_status();
}

Result<void> RemotePageStore::delete_file(const String& filename) {
    if (read_only_) {
        return Status(ErrorCode::kInvalidArgument, "read-only remote page store cannot delete files");
    }
    server_->delete_file(filename);
    return Status::ok_status();
}

Vector<PageIOResult> RemotePageStore::read_pages(const Vector<PageReadRequest>& pages) {
    Vector<PageIOResult> results;
    results.reserve(pages.size());
    if (pages.size() <= batch_size_) {
        if (!read_only_ || read_lsn_ == 0) {
            return server_->read_pages(pages);
        }
        for (u32 i = 0; i < pages.size(); i++) {
            if (pages[i].data) {
                bool ok = server_->read_page(pages[i].page_id, read_lsn_, pages[i].data);
                results.push_back(PageIOResult(
                    pages[i].page_id,
                    ok ? Status::ok_status()
                       : Status(ErrorCode::kNotFound, "read_lsn cannot be satisfied")));
            } else {
                results.push_back(PageIOResult(pages[i].page_id,
                                               Status(ErrorCode::kInvalidArgument,
                                                      "null page buffer")));
            }
        }
        return results;
    }

    Vector<PageReadRequest> batch;
    batch.reserve(batch_size_);
    for (u32 i = 0; i < pages.size(); i++) {
        batch.push_back(pages[i]);
        if (batch.size() >= batch_size_) {
            Vector<PageIOResult> batch_results = read_pages(batch);
            for (u32 j = 0; j < batch_results.size(); j++) results.push_back(batch_results[j]);
            batch.clear();
        }
    }
    if (!batch.empty()) {
        Vector<PageIOResult> batch_results = read_pages(batch);
        for (u32 j = 0; j < batch_results.size(); j++) results.push_back(batch_results[j]);
    }
    return results;
}

Vector<PageIOResult> RemotePageStore::write_pages(const Vector<PageWriteRequest>& pages) {
    Vector<PageIOResult> results;
    results.reserve(pages.size());
    auto append_status = [&results](const Vector<PageWriteRequest>& batch, const Status& st) {
        for (u32 i = 0; i < batch.size(); i++) {
            results.push_back(PageIOResult(batch[i].page_id, st));
        }
    };
    if (read_only_) {
        append_status(pages, Status(ErrorCode::kInvalidArgument,
                                    "read-only remote page store cannot write pages"));
        return results;
    }
    if (pages.size() <= batch_size_) {
        return server_->write_pages(pages);
    }

    Vector<PageWriteRequest> batch;
    batch.reserve(batch_size_);
    for (u32 i = 0; i < pages.size(); i++) {
        batch.push_back(pages[i]);
        if (batch.size() >= batch_size_) {
            Vector<PageIOResult> batch_results = server_->write_pages(batch);
            for (u32 j = 0; j < batch_results.size(); j++) results.push_back(batch_results[j]);
            batch.clear();
        }
    }
    if (!batch.empty()) {
        Vector<PageIOResult> batch_results = server_->write_pages(batch);
        for (u32 j = 0; j < batch_results.size(); j++) results.push_back(batch_results[j]);
    }
    return results;
}

void RemotePageStore::set_durable_lsn(LSN durable_lsn) {
    server_->set_durable_lsn(durable_lsn);
}

LSN RemotePageStore::durable_lsn() const {
    return server_->durable_lsn();
}

} // namespace minidb
