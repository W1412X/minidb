/**
 * @file page_server_tcp.h
 * @brief TCP front-end for PageServer.
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "storage/page_server.h"
#include <atomic>
#include <thread>
#include <vector>

namespace minidb {

class PageServerTcpService : NonCopyable {
public:
    PageServerTcpService(PageServer* server, const String& listen_host, u16 port,
                         u32 max_connections = 1024, u32 io_timeout_ms = 5000);
    ~PageServerTcpService();

    bool start();
    void stop();
    void serve_forever();
    u16 port() const { return port_; }

private:
    void accept_loop();
    void handle_client(int fd);

    PageServer* server_;
    String listen_host_;
    u16 port_;
    u32 max_connections_;
    u32 io_timeout_ms_;
    int listen_fd_;
    std::atomic<bool> running_;
    std::atomic<u32> active_connections_;
    std::thread accept_thread_;
    mutable Mutex workers_latch_;
    mutable Mutex clients_latch_;
    std::vector<std::thread> worker_threads_;
    std::vector<int> active_client_fds_;
};

} // namespace minidb
