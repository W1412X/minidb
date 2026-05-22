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

// Record layout: 8 bytes xid + 1 byte state = 9 bytes. Fixed-size makes
// recovery a single sequential read.
struct __attribute__((packed)) TxnStatusRecord {
    u64 xid;
    u8  state;
};

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
    TxnStatusRecord rec;
    while (read(read_fd, &rec, sizeof(rec)) == static_cast<ssize_t>(sizeof(rec))) {
        states_.insert(rec.xid, rec.state);
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
    TxnStatusRecord rec;
    rec.xid = xid;
    rec.state = static_cast<u8>(state);
    if (write(fd_, &rec, sizeof(rec)) != static_cast<ssize_t>(sizeof(rec))) {
        return false;
    }
    if (fsync(fd_) != 0) return false;
    states_.insert(rec.xid, rec.state);
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
