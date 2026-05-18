#include "storage/page_server.h"
#include "storage/page_server_tcp.h"
#include "storage/page_store.h"
#include "storage/remote_page_store_client.h"
#include "storage/page.h"
#include "storage/disk_manager.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <unistd.h>

using namespace minidb;

static String make_temp_dir(const char* pattern) {
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "%s/%s", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", pattern);
    char* path = mkdtemp(tmpl);
    assert(path != nullptr);
    return String(path);
}

static void fill_page(Page* page, PageId page_id, LSN lsn, byte marker) {
    page->init(page_id, PageType::kHeapData);
    page->header()->lsn = lsn;
    std::memset(page->data() + sizeof(PageHeader), marker, kPageSize - sizeof(PageHeader));
}

static void assert_local_page_store_roundtrip() {
    String dir = make_temp_dir("minidb-local-store.XXXXXX");
    DiskManager disk(dir, true, true, 16);
    LocalPageStore store(&disk);
    Page page;
    PageId pid = make_page_id(11, 1);
    fill_page(&page, pid, 0, 0x11);
    store.write_page(pid, page.data(), page.header()->lsn);
    store.flush();

    byte out[kPageSize];
    std::memset(out, 0, sizeof(out));
    store.read_page(pid, out);
    const PageHeader* hdr = reinterpret_cast<const PageHeader*>(out);
    assert(hdr->page_id == pid);
    assert(out[sizeof(PageHeader)] == 0x11);
}

static void assert_remote_page_store_features() {
    String dir = make_temp_dir("minidb-page-server.XXXXXX");
    PageServer server(dir, true, true, 32, 2);
    RemotePageStore rw(&server, false, 0);
    PageId pid = make_page_id(12, 1);

    Page v5;
    fill_page(&v5, pid, 5, 0x55);
    server.set_durable_lsn(5);
    rw.write_page(pid, v5.data(), v5.header()->lsn);
    rw.flush();
    assert(server.latest_page_lsn(pid) == 5);
    assert(server.log_index_size(pid) == 1);

    Page v10;
    fill_page(&v10, pid, 10, 0xaa);
    bool rejected = server.write_page(pid, v10.data(), 10);
    assert(!rejected);
    assert(server.latest_page_lsn(pid) == 5);

    server.set_durable_lsn(10);
    rw.write_page(pid, v10.data(), v10.header()->lsn);
    assert(server.latest_page_lsn(pid) == 10);
    assert(server.log_index_size(pid) == 2);

    byte latest[kPageSize];
    std::memset(latest, 0, sizeof(latest));
    rw.read_page(pid, latest);
    assert(reinterpret_cast<PageHeader*>(latest)->lsn == 10);
    assert(latest[sizeof(PageHeader)] == 0xaa);

    RemotePageStore ro_at_5(&server, true, 5);
    byte snapshot[kPageSize];
    std::memset(snapshot, 0, sizeof(snapshot));
    ro_at_5.read_page(pid, snapshot);
    assert(reinterpret_cast<PageHeader*>(snapshot)->lsn == 5);
    assert(snapshot[sizeof(PageHeader)] == 0x55);

    Page ignored;
    fill_page(&ignored, pid, 15, 0x15);
    ro_at_5.write_page(pid, ignored.data(), ignored.header()->lsn);
    assert(server.latest_page_lsn(pid) == 10);
    assert(server.replica_count() == 2);
}

static void assert_batch_io() {
    String dir = make_temp_dir("minidb-page-batch.XXXXXX");
    PageServer server(dir, true, true, 32, 1);
    RemotePageStore store(&server, false, 0);
    server.set_durable_lsn(100);

    Page p1;
    Page p2;
    PageId id1 = make_page_id(13, 1);
    PageId id2 = make_page_id(13, 2);
    fill_page(&p1, id1, 20, 0x20);
    fill_page(&p2, id2, 30, 0x30);

    Vector<PageWriteRequest> writes;
    writes.push_back(PageWriteRequest(id1, p1.data(), p1.header()->lsn));
    writes.push_back(PageWriteRequest(id2, p2.data(), p2.header()->lsn));
    store.write_pages(writes);

    byte out1[kPageSize];
    byte out2[kPageSize];
    std::memset(out1, 0, sizeof(out1));
    std::memset(out2, 0, sizeof(out2));
    Vector<PageReadRequest> reads;
    reads.push_back(PageReadRequest(id1, out1));
    reads.push_back(PageReadRequest(id2, out2));
    store.read_pages(reads);
    assert(reinterpret_cast<PageHeader*>(out1)->lsn == 20);
    assert(reinterpret_cast<PageHeader*>(out2)->lsn == 30);
    assert(out1[sizeof(PageHeader)] == 0x20);
    assert(out2[sizeof(PageHeader)] == 0x30);
}

static void assert_log_index_survives_restart() {
    String dir = make_temp_dir("minidb-page-restart.XXXXXX");
    PageId pid = make_page_id(14, 1);
    {
        PageServer server(dir, true, true, 32, 0);
        server.set_durable_lsn(20);
        Page old_page;
        Page new_page;
        fill_page(&old_page, pid, 5, 0x05);
        fill_page(&new_page, pid, 20, 0x20);
        assert(server.write_page(pid, old_page.data(), 5));
        assert(server.write_page(pid, new_page.data(), 20));
        server.flush();
        assert(server.log_index_size(pid) == 2);
    }
    {
        PageServer server(dir, true, true, 32, 0);
        assert(server.log_index_size(pid) == 2);
        assert(server.latest_page_lsn(pid) == 20);
        byte snapshot[kPageSize];
        std::memset(snapshot, 0, sizeof(snapshot));
        server.read_page(pid, 5, snapshot);
        assert(reinterpret_cast<PageHeader*>(snapshot)->lsn == 5);
        assert(snapshot[sizeof(PageHeader)] == 0x05);
    }
}

static void assert_tcp_remote_page_store_client() {
    String dir = make_temp_dir("minidb-page-tcp.XXXXXX");
    PageServer server(dir, true, true, 32, 1);
    PageServerTcpService service(&server, "127.0.0.1", 0, 16, 3000);
    assert(service.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RemotePageStoreClient client("127.0.0.1", service.port(), false, 0, 2, 1000, 3000, 1, 2);
    PageId pid = make_page_id(15, 1);
    Page page;
    fill_page(&page, pid, 7, 0x77);
    client.set_durable_lsn(7);
    client.write_page(pid, page.data(), 7);
    client.flush();

    byte out[kPageSize];
    std::memset(out, 0, sizeof(out));
    client.read_page(pid, out);
    assert(reinterpret_cast<PageHeader*>(out)->lsn == 7);
    assert(out[sizeof(PageHeader)] == 0x77);
    assert(server.latest_page_lsn(pid) == 7);
    service.stop();
}

int main() {
    assert_local_page_store_roundtrip();
    assert_remote_page_store_features();
    assert_batch_io();
    assert_log_index_survives_restart();
    assert_tcp_remote_page_store_client();
    return 0;
}
