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
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <unistd.h>

using namespace minidb;

static void check(bool cond, const char* expr, const char* file, int line) {
    if (cond) return;
    std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", file, line, expr);
    std::fflush(stderr);
    std::abort();
}

#undef assert
#define assert(expr) check((expr), #expr, __FILE__, __LINE__)

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

static int connect_tcp(const char* host, u16 port) {
    char port_buf[16];
    std::snprintf(port_buf, sizeof(port_buf), "%u", port);
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, port_buf, &hints, &res) != 0) return -1;

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
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
    assert(reinterpret_cast<const PageHeader*>(out)->page_id == pid);
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
    assert(!server.write_page(pid, v10.data(), 10));
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

static void assert_log_index_binary_search_boundaries() {
    String dir = make_temp_dir("minidb-page-bsearch.XXXXXX");
    PageServer server(dir, true, true, 32, 0, 2);
    PageId pid = make_page_id(16, 1);

    server.set_durable_lsn(50);
    Page p20;
    Page p10;
    Page p30;
    fill_page(&p20, pid, 20, 0x20);
    fill_page(&p10, pid, 10, 0x10);
    fill_page(&p30, pid, 30, 0x30);

    assert(server.write_page(pid, p20.data(), 20));
    assert(server.write_page(pid, p10.data(), 10));
    assert(server.write_page(pid, p30.data(), 30));
    assert(server.log_index_size(pid) == 3);
    assert(server.latest_page_lsn(pid) == 30);
    assert(server.cached_versions_per_page() == 2);
    assert(server.cached_version_count(pid) == 2);

    byte before_first[kPageSize];
    std::memset(before_first, 0x7f, sizeof(before_first));
    server.read_page(pid, 5, before_first);
    assert(reinterpret_cast<const PageHeader*>(before_first)->lsn == 0);

    byte exact[kPageSize];
    std::memset(exact, 0, sizeof(exact));
    server.read_page(pid, 10, exact);
    assert(reinterpret_cast<PageHeader*>(exact)->lsn == 10);
    assert(exact[sizeof(PageHeader)] == 0x10);

    byte between[kPageSize];
    std::memset(between, 0, sizeof(between));
    server.read_page(pid, 25, between);
    assert(reinterpret_cast<PageHeader*>(between)->lsn == 20);
    assert(between[sizeof(PageHeader)] == 0x20);

    byte after_last[kPageSize];
    std::memset(after_last, 0, sizeof(after_last));
    server.read_page(pid, 35, after_last);
    assert(reinterpret_cast<PageHeader*>(after_last)->lsn == 30);
    assert(after_last[sizeof(PageHeader)] == 0x30);

    byte evicted_but_reconstructable[kPageSize];
    std::memset(evicted_but_reconstructable, 0, sizeof(evicted_but_reconstructable));
    server.read_page(pid, 10, evicted_but_reconstructable);
    assert(reinterpret_cast<PageHeader*>(evicted_but_reconstructable)->lsn == 10);
    assert(evicted_but_reconstructable[sizeof(PageHeader)] == 0x10);
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

static void assert_page_server_stats_are_thread_safe() {
    String dir = make_temp_dir("minidb-page-stats.XXXXXX");
    PageServer server(dir, true, true, 32, 0);
    server.set_durable_lsn(1000);

    const u32 worker_count = 4;
    const u32 iterations = 32;
    std::thread workers[worker_count];
    for (u32 w = 0; w < worker_count; w++) {
        workers[w] = std::thread([&server, w]() {
            for (u32 i = 0; i < iterations; i++) {
                PageId pid = make_page_id(20 + w, i + 1);
                Page page;
                fill_page(&page, pid, static_cast<LSN>(100 + w * iterations + i),
                          static_cast<byte>(0x40 + w));
                assert(server.write_page(pid, page.data(), page.header()->lsn));
                byte out[kPageSize];
                std::memset(out, 0, sizeof(out));
                server.read_page(pid, out);
                assert(reinterpret_cast<PageHeader*>(out)->lsn == page.header()->lsn);
                assert(out[sizeof(PageHeader)] == static_cast<byte>(0x40 + w));
            }
        });
    }
    for (u32 w = 0; w < worker_count; w++) workers[w].join();

    PageServerStats stats = server.stats();
    const u64 expected = static_cast<u64>(worker_count) * iterations;
    assert(stats.write_ops == expected);
    assert(stats.read_ops == expected);
    assert(stats.rejected_writes == 0);
}

static void assert_tcp_stop_joins_active_handlers() {
    String dir = make_temp_dir("minidb-page-stop.XXXXXX");
    PageServer server(dir, true, true, 32, 0);
    PageServerTcpService service(&server, "127.0.0.1", 0, 16, 3000);
    assert(service.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int fd = connect_tcp("127.0.0.1", service.port());
    assert(fd >= 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bool stopped = false;
    std::thread stopper([&service, &stopped]() {
        service.stop();
        stopped = true;
    });
    stopper.join();
    assert(stopped);

    char byte = 0;
    ssize_t n = ::recv(fd, &byte, 1, 0);
    assert(n <= 0);
    ::close(fd);
}

int main() {
    assert_local_page_store_roundtrip();
    assert_remote_page_store_features();
    assert_batch_io();
    assert_log_index_survives_restart();
    assert_log_index_binary_search_boundaries();
    assert_tcp_remote_page_store_client();
    assert_page_server_stats_are_thread_safe();
    assert_tcp_stop_joins_active_handlers();
    return 0;
}
