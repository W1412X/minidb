#include "storage/page_store.h"

namespace minidb {

void PageStore::read_pages(const Vector<PageReadRequest>& pages) {
    for (u32 i = 0; i < pages.size(); i++) {
        if (pages[i].data) read_page(pages[i].page_id, pages[i].data);
    }
}

void PageStore::write_pages(const Vector<PageWriteRequest>& pages) {
    for (u32 i = 0; i < pages.size(); i++) {
        if (pages[i].data) write_page(pages[i].page_id, pages[i].data, pages[i].page_lsn);
    }
}

} // namespace minidb
