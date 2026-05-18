/**
 * @file page_store.h
 * @brief Pluggable page storage backend for local and shared-storage modes.
 */
#pragma once

#include "common/defs.h"
#include "common/noncopyable.h"
#include "container/vector.h"
#include "storage/disk_manager.h"

namespace minidb {

struct PageReadRequest {
    PageId page_id;
    byte* data;
    PageReadRequest() : page_id(kNullPageId), data(nullptr) {}
    PageReadRequest(PageId pid, byte* out) : page_id(pid), data(out) {}
};

struct PageWriteRequest {
    PageId page_id;
    const byte* data;
    LSN page_lsn;
    PageWriteRequest() : page_id(kNullPageId), data(nullptr), page_lsn(0) {}
    PageWriteRequest(PageId pid, const byte* bytes, LSN lsn)
        : page_id(pid), data(bytes), page_lsn(lsn) {}
};

class PageStore : NonCopyable {
public:
    virtual ~PageStore() = default;

    virtual void read_page(PageId page_id, byte* page_data) = 0;
    virtual void write_page(PageId page_id, const byte* page_data, LSN page_lsn) = 0;
    virtual void flush() = 0;
    virtual void delete_file(const String& filename) = 0;

    virtual void read_pages(const Vector<PageReadRequest>& pages);
    virtual void write_pages(const Vector<PageWriteRequest>& pages);
    virtual void set_durable_lsn(LSN durable_lsn) { (void)durable_lsn; }
    virtual LSN durable_lsn() const { return 0; }
    virtual bool is_remote() const { return false; }
};

class LocalPageStore : public PageStore {
public:
    explicit LocalPageStore(DiskManager* disk_mgr) : disk_mgr_(disk_mgr) {}

    void read_page(PageId page_id, byte* page_data) override {
        disk_mgr_->read_page(page_id, page_data);
    }
    void write_page(PageId page_id, const byte* page_data, LSN page_lsn) override {
        (void)page_lsn;
        disk_mgr_->write_page(page_id, page_data);
    }
    void flush() override { disk_mgr_->flush(); }
    void delete_file(const String& filename) override { disk_mgr_->delete_file(filename); }

private:
    DiskManager* disk_mgr_;
};

} // namespace minidb
