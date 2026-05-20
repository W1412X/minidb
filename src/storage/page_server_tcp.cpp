#include "storage/page_server_tcp.h"
#include "storage/page_server_rpc.h"
#include "storage/page.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <array>
#include <chrono>
#include <thread>
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

static void shutdown_fd(int fd) {
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

PageServerTcpService::PageServerTcpService(PageServer* server, const String& listen_host,
                                           u16 port, u32 max_connections, u32 io_timeout_ms)
    : server_(server), listen_host_(listen_host), port_(port),
      max_connections_(max_connections == 0 ? 1 : max_connections),
      io_timeout_ms_(io_timeout_ms == 0 ? 1 : io_timeout_ms),
      listen_fd_(-1), running_(false), active_connections_(0) {}

PageServerTcpService::~PageServerTcpService() {
    stop();
}

bool PageServerTcpService::start() {
    char port_buf[16];
    std::snprintf(port_buf, sizeof(port_buf), "%u", port_);
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    const char* host = listen_host_.empty() ? nullptr : listen_host_.c_str();
    if (getaddrinfo(host, port_buf, &hints, &res) != 0) return false;

    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        listen_fd_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listen_fd_ < 0) continue;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (::bind(listen_fd_, ai->ai_addr, ai->ai_addrlen) == 0 &&
            ::listen(listen_fd_, 128) == 0) {
            break;
        }
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    freeaddrinfo(res);
    if (listen_fd_ < 0) return false;
    if (port_ == 0) {
        sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            if (addr.ss_family == AF_INET) {
                port_ = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
            } else if (addr.ss_family == AF_INET6) {
                port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
            }
        }
    }
    running_.store(true);
    accept_thread_ = std::thread(&PageServerTcpService::accept_loop, this);
    return true;
}

void PageServerTcpService::stop() {
    bool was_running = running_.exchange(false);
    if (listen_fd_ >= 0) {
        shutdown_fd(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (was_running) {
        LockGuard clients_guard(clients_latch_);
        for (size_t i = 0; i < active_client_fds_.size(); i++) {
            shutdown_fd(active_client_fds_[i]);
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    {
        LockGuard workers_guard(workers_latch_);
        for (size_t i = 0; i < worker_threads_.size(); i++) {
            if (worker_threads_[i].joinable()) worker_threads_[i].join();
        }
        worker_threads_.clear();
    }
}

void PageServerTcpService::serve_forever() {
    if (!start()) return;
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void PageServerTcpService::accept_loop() {
    while (running_.load()) {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (active_connections_.load() >= max_connections_) {
            ::close(fd);
            continue;
        }
        active_connections_.fetch_add(1);
        LockGuard workers_guard(workers_latch_);
        worker_threads_.emplace_back([this, fd]() {
            set_timeouts(fd, io_timeout_ms_);
            {
                LockGuard clients_guard(clients_latch_);
                active_client_fds_.push_back(fd);
            }
            handle_client(fd);
            {
                LockGuard clients_guard(clients_latch_);
                for (size_t i = 0; i < active_client_fds_.size(); i++) {
                    if (active_client_fds_[i] == fd) {
                        active_client_fds_.erase(active_client_fds_.begin() + i);
                        break;
                    }
                }
            }
            ::close(fd);
            active_connections_.fetch_sub(1);
        });
    }
}

void PageServerTcpService::handle_client(int fd) {
    while (running_.load()) {
        PageRpcHeader hdr;
        if (!read_full(fd, &hdr, sizeof(hdr))) return;
        if (hdr.magic != kPageRpcMagic || hdr.version != kPageRpcVersion) return;

        PageRpcResponse resp;
        resp.magic = kPageRpcMagic;
        resp.version = kPageRpcVersion;
        resp.status = static_cast<u16>(PageRpcStatus::kOk);
        resp.count = hdr.count;
        resp.reserved = 0;
        resp.durable_lsn = server_->durable_lsn();
        resp.value = 0;

        if (hdr.op == static_cast<u16>(PageRpcOp::kReadBatch)) {
            Vector<PageRpcPageRef> refs;
            refs.resize(hdr.count);
            for (u32 i = 0; i < hdr.count; i++) {
                if (!read_full(fd, &refs[i], sizeof(PageRpcPageRef))) return;
            }
            Vector<std::array<byte, kPageSize>> pages;
            pages.resize(hdr.count);
            for (u32 i = 0; i < hdr.count; i++) {
                bool ok = true;
                if (hdr.read_lsn != 0) {
                    ok = server_->read_page(refs[i].page_id, hdr.read_lsn, pages[i].data());
                } else {
                    server_->read_page(refs[i].page_id, pages[i].data());
                }
                if (!ok) resp.status = static_cast<u16>(PageRpcStatus::kError);
            }
            if (!write_full(fd, &resp, sizeof(resp))) return;
            if (resp.status != static_cast<u16>(PageRpcStatus::kOk)) return;
            for (u32 i = 0; i < hdr.count; i++) {
                if (!write_full(fd, pages[i].data(), kPageSize)) return;
            }
        } else if (hdr.op == static_cast<u16>(PageRpcOp::kWriteBatch)) {
            server_->set_durable_lsn(hdr.durable_lsn);
            byte page[kPageSize];
            for (u32 i = 0; i < hdr.count; i++) {
                PageRpcPageRef ref;
                if (!read_full(fd, &ref, sizeof(ref)) || !read_full(fd, page, kPageSize)) return;
                if (!server_->write_page(ref.page_id, page, ref.page_lsn)) {
                    resp.status = static_cast<u16>(PageRpcStatus::kRejected);
                }
            }
            resp.durable_lsn = server_->durable_lsn();
            if (!write_full(fd, &resp, sizeof(resp))) return;
        } else if (hdr.op == static_cast<u16>(PageRpcOp::kFlush)) {
            server_->set_durable_lsn(hdr.durable_lsn);
            server_->flush();
            resp.durable_lsn = server_->durable_lsn();
            if (!write_full(fd, &resp, sizeof(resp))) return;
        } else if (hdr.op == static_cast<u16>(PageRpcOp::kSetDurableLsn)) {
            server_->set_durable_lsn(hdr.durable_lsn);
            resp.durable_lsn = server_->durable_lsn();
            if (!write_full(fd, &resp, sizeof(resp))) return;
        } else if (hdr.op == static_cast<u16>(PageRpcOp::kDeleteFile)) {
            Vector<char> name_buf;
            name_buf.resize(hdr.name_len + 1);
            if (hdr.name_len > 0 && !read_full(fd, name_buf.data(), hdr.name_len)) return;
            name_buf[hdr.name_len] = '\0';
            server_->delete_file(String(name_buf.data()));
            if (!write_full(fd, &resp, sizeof(resp))) return;
        } else if (hdr.op == static_cast<u16>(PageRpcOp::kStats)) {
            PageServerStats stats = server_->stats();
            resp.value = stats.wal_image_bytes;
            resp.durable_lsn = server_->durable_lsn();
            if (!write_full(fd, &resp, sizeof(resp))) return;
        } else {
            resp.status = static_cast<u16>(PageRpcStatus::kError);
            if (!write_full(fd, &resp, sizeof(resp))) return;
        }
    }
}

} // namespace minidb
