/**
 * @file remote_page_store_client.h
 * @brief TCP PageStore client for independent PageServer processes.
 */
#pragma once

#include "common/mutex.h"
#include "container/string.h"
#include "container/vector.h"
#include "storage/page_store.h"

namespace minidb {

struct RemotePageStoreClientStats {
    u64 read_batches = 0;
    u64 write_batches = 0;
    u64 retries = 0;
    u64 reconnects = 0;
    u64 failures = 0;
};

class RemotePageStoreClient : public PageStore {
public:
    RemotePageStoreClient(const String& host, u16 port, bool read_only = false,
                          LSN read_lsn = 0, u32 batch_size = 64,
                          u32 connect_timeout_ms = 1000, u32 io_timeout_ms = 5000,
                          u32 retry_count = 2, u32 max_connections = 8);
    ~RemotePageStoreClient();

    Result<void> read_page(PageId page_id, byte* page_data) override;
    Result<void> write_page(PageId page_id, const byte* page_data, LSN page_lsn) override;
    Result<void> flush() override;
    Result<void> delete_file(const String& filename) override;
    Vector<PageIOResult> read_pages(const Vector<PageReadRequest>& pages) override;
    Vector<PageIOResult> write_pages(const Vector<PageWriteRequest>& pages) override;
    void set_durable_lsn(LSN durable_lsn) override;
    LSN durable_lsn() const override { return durable_lsn_; }
    bool is_remote() const override { return true; }
    RemotePageStoreClientStats stats() const;

private:
    int connect_once();
    int borrow_connection();
    void release_connection(int fd, bool reusable);
    bool rpc_read_batch(const Vector<PageReadRequest>& pages);
    bool rpc_write_batch(const Vector<PageWriteRequest>& pages);
    bool rpc_simple(u16 op, const String* name = nullptr, LSN lsn = 0);
    bool with_retry(bool (RemotePageStoreClient::*fn_read)(const Vector<PageReadRequest>&),
                    const Vector<PageReadRequest>& pages);
    bool with_retry(bool (RemotePageStoreClient::*fn_write)(const Vector<PageWriteRequest>&),
                    const Vector<PageWriteRequest>& pages);

    String host_;
    u16 port_;
    bool read_only_;
    LSN read_lsn_;
    LSN durable_lsn_;
    u32 batch_size_;
    u32 connect_timeout_ms_;
    u32 io_timeout_ms_;
    u32 retry_count_;
    u32 max_connections_;
    mutable Mutex latch_;
    Vector<int> idle_;
    RemotePageStoreClientStats stats_;
};

} // namespace minidb
