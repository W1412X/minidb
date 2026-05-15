/**
 * @file server.h
 * @brief TCP network server — multi-client SQL connections (thread-per-connection)
 */
#pragma once

#include "common/defs.h"
#include "database/database.h"
#include "container/string.h"
#include "container/hash_map.h"
#include "common/mutex.h"
#include <thread>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace minidb {

enum class StmtType;
struct PlanNode;

class Server {
public:
    Server(Database& db, u16 port = 5433);
    ~Server();

    void start();
    void stop();
    String execute_sql(const String& sql);

    // Streaming execution: write each result row immediately to fd, no in-memory buffering
    // Return row count
    u64 execute_sql_streaming(const String& sql, int fd);

private:
    void handle_client(int client_fd);
    void worker_loop();
    bool enqueue_client(int client_fd);
    bool send_response(int client_fd, const String& response);
    String execute_plan_result(StmtType type, PlanNode* plan);

    Database& db_;
    u16 port_;
    int server_fd_;
    bool running_;
    RwLock exec_latch_;
    Mutex clients_latch_;
    HashMap<String, String> prepared_stmts_;  // W17: name → SQL
    std::vector<int> active_clients_;
    std::vector<std::thread> client_workers_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<int> pending_clients_;
};

} // namespace minidb
