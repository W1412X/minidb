/**
 * @file status.h
 * @brief Unified error State and Result<T>
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"

namespace minidb {

enum class ErrorCode : u16 {
    kOk = 0,
    kNotFound,
    kAlreadyExists,
    kInvalidArgument,
    kIOError,
    kOutOfMemory,
    kBufferFull,
    kPageFull,
    kCorruption,
    kNotImplemented,
    kInternal,
    kAborted,
    kDeadlock,
    kLockConflict,
    kTxnConflict,
};

class Status {
public:
    Status() noexcept : code_(ErrorCode::kOk) {}
    Status(ErrorCode code) noexcept : code_(code) {}
    Status(ErrorCode code, const String& msg) : code_(code), msg_(msg) {}
    Status(ErrorCode code, const char* msg) : code_(code), msg_(msg) {}

    bool ok() const noexcept { return code_ == ErrorCode::kOk; }
    ErrorCode code() const noexcept { return code_; }
    const String& message() const { return msg_; }
    const char* message_cstr() const { return msg_.c_str(); }

    static Status ok_status() { return Status(); }

    bool operator==(const Status& o) const { return code_ == o.code_; }
    bool operator!=(const Status& o) const { return code_ != o.code_; }

private:
    ErrorCode code_;
    String msg_;
};

template<typename T>
class Result {
public:
    Result(T val) : val_(static_cast<T&&>(val)), ok_(true) {}
    Result(Status err) : err_(err), ok_(false) {}

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    Result(Result&& o) noexcept : ok_(o.ok_) {
        if (ok_) new (&val_) T(static_cast<T&&>(o.val_));
        else new (&err_) Status(static_cast<Status&&>(o.err_));
    }

    ~Result() {
        if (ok_) val_.~T();
        else err_.~Status();
    }

    bool ok() const { return ok_; }
    T& value() { return val_; }
    const T& value() const { return val_; }
    Status error() const { return err_; }

    T& operator*() { return val_; }
    T* operator->() { return &val_; }

private:
    union {
        T val_;
        Status err_;
    };
    bool ok_;
};

} // namespace minidb
