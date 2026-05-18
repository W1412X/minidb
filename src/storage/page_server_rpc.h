/**
 * @file page_server_rpc.h
 * @brief Binary RPC protocol shared by PageServer TCP service and RemotePageStoreClient.
 */
#pragma once

#include "common/defs.h"

namespace minidb {

static constexpr u32 kPageRpcMagic = 0x4D445250u; // "MDRP"
static constexpr u16 kPageRpcVersion = 1;

enum class PageRpcOp : u16 {
    kReadBatch = 1,
    kWriteBatch = 2,
    kFlush = 3,
    kDeleteFile = 4,
    kSetDurableLsn = 5,
    kStats = 6,
};

enum class PageRpcStatus : u16 {
    kOk = 0,
    kError = 1,
    kRejected = 2,
};

#pragma pack(push, 1)
struct PageRpcHeader {
    u32 magic;
    u16 version;
    u16 op;
    u32 count;
    u32 name_len;
    u64 read_lsn;
    u64 durable_lsn;
};

struct PageRpcResponse {
    u32 magic;
    u16 version;
    u16 status;
    u32 count;
    u32 reserved;
    u64 durable_lsn;
    u64 value;
};

struct PageRpcPageRef {
    PageId page_id;
    LSN page_lsn;
};
#pragma pack(pop)

} // namespace minidb
