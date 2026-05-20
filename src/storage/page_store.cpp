#include "storage/page_store.h"

namespace minidb {

Vector<PageIOResult> PageStore::read_pages(const Vector<PageReadRequest>& pages) {
    Vector<PageIOResult> results;
    results.reserve(pages.size());
    for (u32 i = 0; i < pages.size(); i++) {
        Status st = pages[i].data
            ? read_page(pages[i].page_id, pages[i].data).error()
            : Status(ErrorCode::kInvalidArgument, "null page buffer");
        results.push_back(PageIOResult(pages[i].page_id, st));
    }
    return results;
}

Vector<PageIOResult> PageStore::write_pages(const Vector<PageWriteRequest>& pages) {
    Vector<PageIOResult> results;
    results.reserve(pages.size());
    for (u32 i = 0; i < pages.size(); i++) {
        Status st = pages[i].data
            ? write_page(pages[i].page_id, pages[i].data, pages[i].page_lsn).error()
            : Status(ErrorCode::kInvalidArgument, "null page buffer");
        results.push_back(PageIOResult(pages[i].page_id, st));
    }
    return results;
}

} // namespace minidb
