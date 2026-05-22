/**
 * @file main.cpp
 * @brief MiniDB program entrypoint — single database mode
 */
#include "database/database.h"
#include "common/db_config.h"
#include "repl/repl.h"
#include "network/server.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

int main(int argc, char* argv[]) {
    const char* db_dir = "minidb_data";
    const char* config_path = nullptr;
    bool server_mode = false;
    bool show_config = false;
    int port = 5433;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0) server_mode = true;
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) db_dir = argv[++i];
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "--show-config") == 0) show_config = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: minidb [options]\n");
            printf("  --server          Run as TCP server\n");
            printf("  --port <port>     Server port (default 5433)\n");
            printf("  --dir <path>      Database directory (default minidb_data)\n");
            printf("  --config <path>   Runtime config file (default <dir>/minidb.conf)\n");
            printf("  --show-config     Print effective config and exit\n");
            printf("  --help            Show this help\n");
            return 0;
        }
    }

    minidb::DbConfig config;
    minidb::String cfg_path = config_path ? minidb::String(config_path)
                                          : (minidb::String(db_dir) + "/minidb.conf");
    minidb::String cfg_error;
    if (!minidb::DbConfigLoader::load_file(cfg_path, &config, &cfg_error)) {
        fprintf(stderr, "Config error: %s\n", cfg_error.c_str());
        return 1;
    }
    if (port != 5433) {
        config.port = static_cast<minidb::u16>(port);
    } else {
        port = config.port;
    }
    if (show_config) {
        printf("%s", minidb::DbConfigLoader::describe(config).c_str());
        return 0;
    }

    minidb::Database db(db_dir, config);

    if (server_mode) {
        printf("MiniADB v0.3.0 — Server Mode\n");
        printf("Data directory: %s\n", db_dir);
        minidb::Server server(db, static_cast<minidb::u16>(config.port));
        server.start();
    } else {
        printf("Data directory: %s\n", db_dir);
        minidb::REPL repl(db);
        repl.run();
    }

    return 0;
}
