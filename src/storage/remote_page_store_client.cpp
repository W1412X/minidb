#include "storage/remote_page_store_client.h"
#include "storage/page.h"
#include "storage/page_server_rpc.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace minidb {

static bool read_full(int fd, void* data, size_t len) {
    byte* p = static_cast<byte*>(data);
    while (len > 0) {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static bool write_full(int fd, const void* data, size_t len) {
    const byte* p = static_cast<const byte*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static void set_timeouts(int fd, u32 timeout_ms) {
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

RemotePageStoreClient::RemotePageStoreClient(const String& host, u16 port, bool read_only,
                                             LSN read_lsn, u32 batch_size,
                                             u32 connect_timeout_ms, u32 io_timeout_ms,
                                             u32 retry_count, u32 max_connections)
    : host_(host), port_(port), read_only_(read_only), read_lsn_(read_lsn),
      durable_lsn_(0), batch_size_(batch_size == 0 ? 1 : batch_size),
      connect_timeout_ms_(connect_timeout_ms == 0 ? 1 : connect_timeout_ms),
      io_timeout_ms_(io_timeout_ms == 0 ? 1 : io_timeout_ms),
      retry_count_(retry_count), max_connections_(max_connections == 0 ? 1 : max_connections) {}

RemotePageStoreClient::~RemotePageStoreClient() {
    LockGuard guard(latch_);
    for (u32 i = 0; i < idle_.size(); i++) {
        if (idle_[i] >= 0) ::close(idle_[i]);
    }
}

int RemotePageStoreClient::connect_once() {
    char port_buf[16];
    std::snprintf(port_buf, sizeof(port_buf), "%u", port_);
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host_.c_str(), port_buf, &hints, &res) != 0) return -1;

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            ::close(fd);
            fd = -1;
            continue;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv;
        tv.tv_sec = connect_timeout_ms_ / 1000;
        tv.tv_usec = static_cast<suseconds_t>((connect_timeout_ms_ % 1000) * 1000);
        rc = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (rc > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) {
                fcntl(fd, F_SETFL, flags);
                set_timeouts(fd, io_timeout_ms_);
                break;
            }
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int RemotePageStoreClient::borrow_connection() {
    {
        LockGuard guard(latch_);
        if (!idle_.empty()) {
            int fd = idle_.back();
            idle_.pop_back();
            return fd;
        }
    }
    int fd = connect_once();
    LockGuard guard(latch_);
    if (fd >= 0) stats_.reconnects++;
    return fd;
}

void RemotePageStoreClient::release_connection(int fd, bool reusable) {
    if (fd < 0) return;
    LockGuard guard(latch_);
    if (reusable && idle_.size() < max_connections_) {
        idle_.push_back(fd);
        return;
    }
    ::close(fd);
}

bool RemotePageStoreClient::rpc_read_batch(const Vector<PageReadRequest>& pages) {
    int fd = borrow_connection();
    if (fd < 0) return false;
    PageRpcHeader hdr;
    hdr.magic = kPageRpcMagic;
    hdr.version = kPageRpcVersion;
    hdr.op = static_cast<u16>(PageRpcOp::kReadBatch);
    hdr.count = pages.size();
    hdr.name_len = 0;
    hdr.read_lsn = read_only_ ? read_lsn_ : 0;
    hdr.durable_lsn = durable_lsn_;
    bool ok = write_full(fd, &hdr, sizeof(hdr));
    for (u32 i = 0; ok && i < pages.size(); i++) {
        PageRpcPageRef ref{pages[i].page_id, 0};
        ok = write_full(fd, &ref, sizeof(ref));
    }
    PageRpcResponse resp;
    ok = ok && read_full(fd, &resp, sizeof(resp)) &&
         resp.magic == kPageRpcMagic && resp.status == static_cast<u16>(PageRpcStatus::kOk) &&
         resp.count == pages.size();
    for (u32 i = 0; ok && i < pages.size(); i++) {
        ok = pages[i].data && read_full(fd, pages[i].data, kPageSize);
    }
    release_connection(fd, ok);
    if (ok) {
        LockGuard guard(latch_);
        stats_.read_batches++;
    }
    return ok;
}

bool RemotePageStoreClient::rpc_write_batch(const Vector<PageWriteRequest>& pages) {
    if (read_only_) return true;
    int fd = borrow_connection();
    if (fd < 0) return false;
    PageRpcHeader hdr;
    hdr.magic = kPageRpcMagic;
    hdr.version = kPageRpcVersion;
    hdr.op = static_cast<u16>(PageRpcOp::kWriteBatch);
    hdr.count = pages.size();
    hdr.name_len = 0;
    hdr.read_lsn = 0;
    hdr.durable_lsn = durable_lsn_;
    bool ok = write_full(fd, &hdr, sizeof(hdr));
    for (u32 i = 0; ok && i < pages.size(); i++) {
        PageRpcPageRef ref{pages[i].page_id, pages[i].page_lsn};
        ok = write_full(fd, &ref, sizeof(ref)) &&
             pages[i].data && write_full(fd, pages[i].data, kPageSize);
    }
    PageRpcResponse resp;
    ok = ok && read_full(fd, &resp, sizeof(resp)) &&
         resp.magic == kPageRpcMagic && resp.status == static_cast<u16>(PageRpcStatus::kOk);
    release_connection(fd, ok);
    if (ok) {
        LockGuard guard(latch_);
        stats_.write_batches++;
    }
    return ok;
}

bool RemotePageStoreClient::rpc_simple(u16 op, const String* name, LSN lsn) {
    int fd = borrow_connection();
    if (fd < 0) return false;
    PageRpcHeader hdr;
    hdr.magic = kPageRpcMagic;
    hdr.version = kPageRpcVersion;
    hdr.op = op;
    hdr.count = 0;
    hdr.name_len = name ? name->size() : 0;
    hdr.read_lsn = 0;
    hdr.durable_lsn = lsn;
    bool ok = write_full(fd, &hdr, sizeof(hdr));
    if (ok && name && name->size() > 0) {
        ok = write_full(fd, name->c_str(), name->size());
    }
    PageRpcResponse resp;
    ok = ok && read_full(fd, &resp, sizeof(resp)) &&
         resp.magic == kPageRpcMagic && resp.status == static_cast<u16>(PageRpcStatus::kOk);
    if (ok) durable_lsn_ = resp.durable_lsn;
    release_connection(fd, ok);
    return ok;
}

bool RemotePageStoreClient::with_retry(bool (RemotePageStoreClient::*fn)(const Vector<PageReadRequest>&),
                                       const Vector<PageReadRequest>& pages) {
    for (u32 i = 0; i <= retry_count_; i++) {
        if ((this->*fn)(pages)) return true;
        LockGuard guard(latch_);
        if (i < retry_count_) stats_.retries++;
        else stats_.failures++;
    }
    return false;
}

bool RemotePageStoreClient::with_retry(bool (RemotePageStoreClient::*fn)(const Vector<PageWriteRequest>&),
                                       const Vector<PageWriteRequest>& pages) {
    for (u32 i = 0; i <= retry_count_; i++) {
        if ((this->*fn)(pages)) return true;
        LockGuard guard(latch_);
        if (i < retry_count_) stats_.retries++;
        else stats_.failures++;
    }
    return false;
}

void RemotePageStoreClient::read_page(PageId page_id, byte* page_data) {
    Vector<PageReadRequest> pages;
    pages.push_back(PageReadRequest(page_id, page_data));
    read_pages(pages);
}

void RemotePageStoreClient::write_page(PageId page_id, const byte* page_data, LSN page_lsn) {
    Vector<PageWriteRequest> pages;
    pages.push_back(PageWriteRequest(page_id, page_data, page_lsn));
    write_pages(pages);
}

void RemotePageStoreClient::flush() {
    (void)rpc_simple(static_cast<u16>(PageRpcOp::kFlush), nullptr, durable_lsn_);
}

void RemotePageStoreClient::delete_file(const String& filename) {
    if (read_only_) return;
    (void)rpc_simple(static_cast<u16>(PageRpcOp::kDeleteFile), &filename, durable_lsn_);
}

void RemotePageStoreClient::read_pages(const Vector<PageReadRequest>& pages) {
    if (pages.size() <= batch_size_) {
        (void)with_retry(&RemotePageStoreClient::rpc_read_batch, pages);
        return;
    }
    Vector<PageReadRequest> batch;
    batch.reserve(batch_size_);
    for (u32 i = 0; i < pages.size(); i++) {
        batch.push_back(pages[i]);
        if (batch.size() >= batch_size_) {
            (void)with_retry(&RemotePageStoreClient::rpc_read_batch, batch);
            batch.clear();
        }
    }
    if (!batch.empty()) (void)with_retry(&RemotePageStoreClient::rpc_read_batch, batch);
}

void RemotePageStoreClient::write_pages(const Vector<PageWriteRequest>& pages) {
    if (read_only_) return;
    if (pages.size() <= batch_size_) {
        (void)with_retry(&RemotePageStoreClient::rpc_write_batch, pages);
        return;
    }
    Vector<PageWriteRequest> batch;
    batch.reserve(batch_size_);
    for (u32 i = 0; i < pages.size(); i++) {
        batch.push_back(pages[i]);
        if (batch.size() >= batch_size_) {
            (void)with_retry(&RemotePageStoreClient::rpc_write_batch, batch);
            batch.clear();
        }
    }
    if (!batch.empty()) (void)with_retry(&RemotePageStoreClient::rpc_write_batch, batch);
}

void RemotePageStoreClient::set_durable_lsn(LSN durable_lsn) {
    durable_lsn_ = durable_lsn;
    (void)rpc_simple(static_cast<u16>(PageRpcOp::kSetDurableLsn), nullptr, durable_lsn);
}

RemotePageStoreClientStats RemotePageStoreClient::stats() const {
    LockGuard guard(latch_);
    return stats_;
}

} // namespace minidb
