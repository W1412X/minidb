/**
 * @file page_store.h
 * @brief Pluggable page storage backend for local and shared-storage modes.
 */
#pragma once

#include "common/defs.h"
#include "common/noncopyable.h"
#include "common/status.h"
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

struct PageIOResult {
    PageId page_id;
    Status status;
    PageIOResult() : page_id(kNullPageId), status(Status::ok_status()) {}
    PageIOResult(PageId pid, const Status& st) : page_id(pid), status(st) {}
    bool ok() const { return status.ok(); }
};

class PageStore : NonCopyable {
public:
    virtual ~PageStore() = default;

    virtual Result<void> read_page(PageId page_id, byte* page_data) = 0;
    virtual Result<void> write_page(PageId page_id, const byte* page_data, LSN page_lsn) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<void> delete_file(const String& filename) = 0;

    virtual Vector<PageIOResult> read_pages(const Vector<PageReadRequest>& pages);
    virtual Vector<PageIOResult> write_pages(const Vector<PageWriteRequest>& pages);
    virtual void set_durable_lsn(LSN durable_lsn) { (void)durable_lsn; }
    virtual LSN durable_lsn() const { return 0; }
    virtual bool is_remote() const { return false; }
};

class LocalPageStore : public PageStore {
public:
    explicit LocalPageStore(DiskManager* disk_mgr) : disk_mgr_(disk_mgr) {}

    Result<void> read_page(PageId page_id, byte* page_data) override {
        if (!page_data) return Status(ErrorCode::kInvalidArgument, "null page buffer");
        disk_mgr_->read_page(page_id, page_data);
        return Status::ok_status();
    }
    Result<void> write_page(PageId page_id, const byte* page_data, LSN page_lsn) override {
        if (!page_data) return Status(ErrorCode::kInvalidArgument, "null page buffer");
        (void)page_lsn;
        disk_mgr_->write_page(page_id, page_data);
        return Status::ok_status();
    }
    Result<void> flush() override {
        disk_mgr_->flush();
        return Status::ok_status();
    }
    Result<void> delete_file(const String& filename) override {
        disk_mgr_->delete_file(filename);
        return Status::ok_status();
    }

private:
    DiskManager* disk_mgr_;
};

} // namespace minidb
