#include "recovery/wal.h"
#include "storage/buffer_pool.h"
#include "storage/page.h"
#include "storage/page_store.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

using namespace minidb;

static String make_temp_dir(const char* pattern) {
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "%s/%s",
                  std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp",
                  pattern);
    char* path = mkdtemp(tmpl);
    assert(path != nullptr);
    return String(path);
}

class RecordingPageStore : public PageStore {
public:
    Result<void> read_page(PageId page_id, byte* page_data) override {
        Page page;
        page.init(page_id, PageType::kHeapData);
        std::memcpy(page_data, page.data(), kPageSize);
        return Status::ok_status();
    }

    Result<void> write_page(PageId page_id, const byte* page_data, LSN page_lsn) override {
        (void)page_data;
        last_write_page_id = page_id;
        last_write_lsn = page_lsn;
        durable_lsn_at_write = durable_lsn_;
        write_count++;
        return Status::ok_status();
    }

    Result<void> flush() override { return Status::ok_status(); }
    Result<void> delete_file(const String& filename) override {
        unlink(filename.c_str());
        return Status::ok_status();
    }
    void set_durable_lsn(LSN durable_lsn) override { durable_lsn_ = durable_lsn; }
    LSN durable_lsn() const override { return durable_lsn_; }

    PageId last_write_page_id = kNullPageId;
    LSN last_write_lsn = 0;
    LSN durable_lsn_at_write = 0;
    u32 write_count = 0;

private:
    LSN durable_lsn_ = 0;
};

int main() {
    String wal_dir = make_temp_dir("minidb-wal-first.XXXXXX");
    WalManager wal(wal_dir, 1024 * 1024, false);
    RecordingPageStore store;
    BufferPool pool(&store, 1, 100, 4, 1, 1);
    pool.set_wal_manager(&wal);

    PageId page1 = make_page_id(30, 1);
    PageId page2 = make_page_id(30, 2);
    auto created = pool.new_page(page1, PageType::kHeapData);
    assert(created.ok());
    LSN dirty_lsn = wal.log_begin(123);
    assert(dirty_lsn != 0);
    pool.set_page_lsn(page1, dirty_lsn);
    pool.unpin_page(page1);

    auto fetched = pool.fetch_page(page2);
    assert(fetched.ok());
    pool.unpin_page(page2);

    assert(store.write_count == 1);
    assert(store.last_write_page_id == page1);
    assert(store.last_write_lsn == dirty_lsn);
    assert(store.durable_lsn_at_write >= dirty_lsn);
    assert(wal.durable_lsn() >= dirty_lsn);

    return 0;
}
