/**
 * @file txn_status_log.h
 * @brief Persistent xid → final-status map (ACID A1).
 *
 * MiniDB's in-memory TxnSlot table only holds the most-recent-N transactions;
 * once a slot is reused, the old transaction's final state is gone. For
 * committed transactions this is harmless — their writes survived and the
 * visibility predicate happily treats "slot-not-found" as committed. For
 * ABORTED transactions whose rollback was interrupted, the slot loss is a
 * correctness problem: a stale xmin would be misread as committed.
 *
 * TxnStatusLog plugs that hole. Every successful commit/rollback appends a
 * fixed-size record to txn_status.log and fsyncs. On startup the file is
 * read into a HashMap so the visibility predicate can fall back to it when
 * the slot table no longer has the answer.
 *
 * The format is intentionally trivial — one entry per record, no segmentation
 * — so the implementation stays under 150 lines. Truncation / segmentation
 * is future work and is safe to skip while transaction throughput is small.
 */
#pragma once

#include "common/defs.h"
#include "common/mutex.h"
#include "container/hash_map.h"
#include "container/string.h"

namespace minidb {

enum class TxnFinalState : u8 {
    kCommitted = 1,
    kAborted   = 2,
};

class TxnStatusLog {
public:
    explicit TxnStatusLog(const String& dir);
    ~TxnStatusLog();

    // Append a record and fsync before returning. Returns false on I/O failure.
    bool record(u64 xid, TxnFinalState state);

    // Returns true if `xid` has a recorded final state; writes it through.
    bool status(u64 xid, TxnFinalState* out) const;

    // Total number of records on disk + in memory.
    u32 size() const;

private:
    String path_;
    int fd_;
    mutable Mutex latch_;
    HashMap<u64, u8> states_;
};

} // namespace minidb
