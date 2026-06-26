// Unit tests for DbConfig file parsing — focus on input validation.
#include "common/db_config.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

using namespace minidb;

static std::string write_temp(const char* contents) {
    std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                       + "/minidb_cfg_test_XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    assert(fd >= 0);
    FILE* f = fdopen(fd, "w");
    std::fputs(contents, f);
    std::fclose(f);
    return std::string(buf.data());
}

static bool load(const char* contents, DbConfig* cfg) {
    std::string path = write_temp(contents);
    bool ok = DbConfigLoader::load_file(String(path.c_str()), cfg, nullptr);
    std::remove(path.c_str());
    return ok;
}

int main() {
    // Valid values with units parse and apply the multiplier.
    {
        DbConfig cfg;
        assert(load("shared_buffers = 64MB\nwork_mem = 1024\n", &cfg));
        assert(cfg.shared_buffers_bytes == 64ull * 1024 * 1024);
        assert(cfg.work_mem_bytes == 1024);
    }
    // Negative values must be rejected, not wrapped to a huge unsigned limit.
    {
        DbConfig cfg;
        assert(!load("max_result_rows = -1\n", &cfg));
    }
    {
        DbConfig cfg;
        assert(!load("shared_buffers = -1MB\n", &cfg));
    }
    // Multiplication overflow must be rejected.
    {
        DbConfig cfg;
        // 100 EB * GB multiplier overflows u64.
        assert(!load("shared_buffers = 100000000000000000000 GB\n", &cfg));
    }
    // A plain huge-but-in-range value still parses.
    {
        DbConfig cfg;
        assert(load("max_result_bytes = 4096\n", &cfg));
        assert(cfg.max_result_bytes == 4096);
    }
    return 0;
}
