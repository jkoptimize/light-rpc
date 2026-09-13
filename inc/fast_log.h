#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/time.h>

// The output backend remains local. The macros and check/rate-limit helpers
// in detail/fast_log_macros.h are ported from brpc; see the port record.
namespace fast {

enum LogLevel { INFO, ERROR, FATAL };

inline std::atomic<int> min_log_level{INFO};

inline int GetMinLogLevel() {
    return min_log_level.load(std::memory_order_relaxed);
}

inline void SetMinLogLevel(int level) {
    // Fatal errors must always be emitted and terminate the process.
    min_log_level.store(level > FATAL ? FATAL : level,
                        std::memory_order_relaxed);
}

class LogMessage {
public:
    explicit LogMessage(LogLevel level) : level_(level) {}
    LogMessage(const char*, int, const char*, LogLevel level)
        : LogMessage(level) {}
    LogMessage(const char* file, int line, const char* func, std::string* result)
        : LogMessage(file, line, func, FATAL, result) {}
    LogMessage(const char* file, int line, const char* func, LogLevel level,
               std::string* result)
        : LogMessage(file, line, func, level) {
        stream_ << "Check failed: " << *result;
        delete result;
    }
    ~LogMessage() {
        std::cerr << stream_.str() << std::endl;
        if (level_ == FATAL) {
            abort();
        }
    }

    std::ostream& stream() { return stream_; }

    template <typename T>
    LogMessage& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }
    LogMessage& operator<<(std::ostream& (*manip)(std::ostream&)) {
        manip(stream_);
        return *this;
    }

    LogMessage(const LogMessage&) = delete;
    LogMessage& operator=(const LogMessage&) = delete;

private:
    LogLevel level_;
    std::ostringstream stream_;
};

// Capture errno before evaluating stream arguments, append it at destruction,
// as in brpc's POSIX ErrnoLogMessage. Symbolization/backtrace is not ported.
class ErrnoLogMessage {
public:
    ErrnoLogMessage(const char* file, int line, const char* func, LogLevel level,
                    int error)
        : error_(error), message_(file, line, func, level) {}
    ~ErrnoLogMessage() {
        stream() << ": " << strerror(error_) << " [errno=" << error_ << ']';
    }
    std::ostream& stream() { return message_.stream(); }

private:
    int error_;
    LogMessage message_;
};

namespace logging_detail {
// Equivalent primitives for brpc's NoBarrier atomic operations. Keep the
// previous-value CAS and new-value increment return conventions.
using Atomic32 = std::atomic<int32_t>;
using Atomic64 = std::atomic<int64_t>;
static_assert(Atomic32::is_always_lock_free && Atomic64::is_always_lock_free,
              "logging rate limits require lock-free atomics");

inline int32_t NoBarrier_AtomicIncrement(Atomic32* ptr, int32_t increment) {
    return ptr->fetch_add(increment, std::memory_order_relaxed) + increment;
}

inline int64_t NoBarrier_CompareAndSwap(Atomic64* ptr, int64_t old_value,
                                      int64_t new_value) {
    ptr->compare_exchange_strong(old_value, new_value,
                                std::memory_order_relaxed,
                                std::memory_order_relaxed);
    return old_value;
}

// Same clock and conversion as butil::gettimeofday_us(). Keep wall-clock
// rollback behavior identical to the upstream EVERY_SECOND macros.
inline int64_t gettimeofday_us() {
    timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec * 1000000L + now.tv_usec;
}
}  // namespace logging_detail
}  // namespace fast

#include "detail/fast_log_macros.h"
