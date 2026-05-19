/**
 * @file pageserver_main.cpp
 * @brief Standalone PageServer process entrypoint.
 */
#include "common/db_config.h"
#include "storage/page_server.h"
#include "storage/page_server_tcp.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

static std::atomic<bool> g_running(true);

static void handle_signal(int) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    const char* data_dir = "minidb_pageserver";
    const char* config_path = nullptr;
    const char* host = "127.0.0.1";
    int port = 15433;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: minidb_pageserver [--dir path] [--host addr] [--port port] [--config file]\n");
            return 0;
        }
    }

    minidb::DbConfig config;
    minidb::String cfg_path = config_path ? minidb::String(config_path)
                                          : (minidb::String(data_dir) + "/pageserver.conf");
    minidb::String cfg_error;
    if (!minidb::DbConfigLoader::load_file(cfg_path, &config, &cfg_error)) {
        std::fprintf(stderr, "Config error: %s\n", cfg_error.c_str());
        return 1;
    }
    if (port <= 0) port = config.page_server_port;

    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    minidb::PageServer server(data_dir, config.doublewrite, config.page_checksum,
                              config.fd_cache_limit, config.page_server_replicas,
                              config.page_server_cached_versions_per_page);
    minidb::PageServerTcpService service(&server, host, static_cast<minidb::u16>(port),
                                         config.page_server_max_connections,
                                         config.remote_io_timeout_ms);
    if (!service.start()) {
        std::fprintf(stderr, "failed to start PageServer on %s:%d\n", host, port);
        return 1;
    }
    std::printf("MiniDB PageServer listening on %s:%d\n", host, port);
    std::fflush(stdout);
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    service.stop();
    server.flush();
    return 0;
}
