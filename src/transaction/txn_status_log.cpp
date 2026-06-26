/**
 * @file txn_status_log.cpp
 * @brief Implementation of the append-only xid -> final state log.
 */
#include "transaction/txn_status_log.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

namespace minidb {

// Record layout on disk: 8 bytes little-endian xid + 1 byte state = 9 bytes.
// Fixed-size makes recovery a single sequential read. We serialize through an
// explicit byte buffer with memcpy rather than a packed struct: binding a
// reference to a packed (alignment-1) member is undefined behavior and crashes
// on strict-alignment targets.
static constexpr size_t kRecordSize = sizeof(u64) + sizeof(u8);

TxnStatusLog::TxnStatusLog(const String& dir)
    : path_(dir + "/txn_status.log"), fd_(-1) {
    mkdir(dir.c_str(), 0755);
    fd_ = open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) return;
    // Replay the whole file into memory. Each entry is fixed-size, so a
    // half-written torn tail is detected by an incomplete read and silently
    // dropped — the live transaction will rewrite its final state on the
    // next commit/abort attempt.
    int read_fd = open(path_.c_str(), O_RDONLY);
    if (read_fd < 0) return;
    byte buf[kRecordSize];
    while (read(read_fd, buf, kRecordSize) == static_cast<ssize_t>(kRecordSize)) {
        u64 xid;
        std::memcpy(&xid, buf, sizeof(xid));
        u8 state = static_cast<u8>(buf[sizeof(u64)]);
        states_.insert(xid, state);
    }
    close(read_fd);
}

TxnStatusLog::~TxnStatusLog() {
    if (fd_ >= 0) {
        fsync(fd_);
        close(fd_);
    }
}

bool TxnStatusLog::record(u64 xid, TxnFinalState state) {
    LockGuard guard(latch_);
    if (fd_ < 0) return false;
    byte buf[kRecordSize];
    std::memcpy(buf, &xid, sizeof(xid));
    buf[sizeof(u64)] = static_cast<byte>(state);
    if (write(fd_, buf, kRecordSize) != static_cast<ssize_t>(kRecordSize)) {
        return false;
    }
    if (fsync(fd_) != 0) return false;
    states_.insert(xid, static_cast<u8>(state));
    return true;
}

bool TxnStatusLog::status(u64 xid, TxnFinalState* out) const {
    LockGuard guard(latch_);
    const u8* s = states_.find(xid);
    if (!s) return false;
    if (out) *out = static_cast<TxnFinalState>(*s);
    return true;
}

u32 TxnStatusLog::size() const {
    LockGuard guard(latch_);
    return states_.size();
}

} // namespace minidb
