// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// Date: 2012-10-08 23:53:50

// Merged chromium log and streaming log.

#pragma once

// Selected blocks from brpc src/butil/logging.h at
// d688e7550be4b4c41b9a4dc55add2a2c75be1296.
// Included through fast_log.h. Adaptations: namespace/prefix, local stream
// backend (no SetCheck annotation), lock-free std::atomic primitives.
// The upstream rate-limit algorithm, call-site state and clock are retained.
// As upstream, rate macros expand to multiple statements: use braces in if/else.

namespace fast {
class LogMessageVoidify {
public:
    LogMessageVoidify() { }
    // This has to be an operator with a precedence lower than << but
    // higher than ?:
    void operator&(std::ostream&) { }
};

template<class t1, class t2>
std::string* MakeCheckOpString(const t1& v1, const t2& v2, const char* names) {
    std::ostringstream ss;
    ss << names << " (" << v1 << " vs " << v2 << "). ";
    std::string* msg = new std::string(ss.str());
    return msg;
}

// Helper functions for FAST_CHECK_OP macro.
// The (int, int) specialization works around the issue that the compiler
// will not instantiate the template version of the function on values of
// unnamed enum type - see comment below.
#define FAST_DEFINE_CHECK_OP_IMPL(name, op)                            \
    template <class t1, class t2>                                       \
    inline std::string* Check##name##Impl(const t1& v1, const t2& v2,   \
                                          const char* names) {          \
        if (v1 op v2) return NULL;                                      \
        else return MakeCheckOpString(v1, v2, names);                   \
    }                                                                   \
    inline std::string* Check##name##Impl(int v1, int v2, const char* names) { \
        if (v1 op v2) return NULL;                                      \
        else return MakeCheckOpString(v1, v2, names);                   \
    }
FAST_DEFINE_CHECK_OP_IMPL(EQ, ==)
FAST_DEFINE_CHECK_OP_IMPL(NE, !=)
FAST_DEFINE_CHECK_OP_IMPL(LE, <=)
FAST_DEFINE_CHECK_OP_IMPL(LT, < )
FAST_DEFINE_CHECK_OP_IMPL(GE, >=)
FAST_DEFINE_CHECK_OP_IMPL(GT, > )
#undef FAST_DEFINE_CHECK_OP_IMPL

}  // namespace fast

#define FAST_CONCAT_IMPL(a, b) a##b
#define FAST_CONCAT(a, b) FAST_CONCAT_IMPL(a, b)
#define LOG_IS_ON(severity) (::fast::severity >= ::fast::GetMinLogLevel())
#define FAST_LOG_STREAM(severity) \
    ::fast::LogMessage(__FILE__, __LINE__, __func__, ::fast::severity).stream()
#define FAST_PLOG_STREAM(severity) \
    ::fast::ErrnoLogMessage(__FILE__, __LINE__, __func__, ::fast::severity, errno).stream()
#define FAST_LAZY_STREAM(stream, condition) \
    !(condition) ? (void)0 : ::fast::LogMessageVoidify() & (stream)
#define LOG(severity) FAST_LAZY_STREAM(FAST_LOG_STREAM(severity), LOG_IS_ON(severity))
#define LOG_IF(severity, condition) \
    FAST_LAZY_STREAM(FAST_LOG_STREAM(severity), LOG_IS_ON(severity) && (condition))
#define PLOG(severity) FAST_LAZY_STREAM(FAST_PLOG_STREAM(severity), LOG_IS_ON(severity))
#define PLOG_IF(severity, condition) \
    FAST_LAZY_STREAM(FAST_PLOG_STREAM(severity), LOG_IS_ON(severity) && (condition))
#define CHECK(condition)                                        \
    FAST_LAZY_STREAM(FAST_LOG_STREAM(FATAL), !(condition))     \
    << "Check failed: " #condition ". "

#define PCHECK(condition)                                       \
    FAST_LAZY_STREAM(FAST_PLOG_STREAM(FATAL), !(condition))    \
    << "Check failed: " #condition ". "

// Helper macro for binary operators.
// Don't use this macro directly in your code, use CHECK_EQ et al below.
//
// TODO(akalin): Rewrite this so that constructs like if (...)
// CHECK_EQ(...) else { ... } work properly.
#define FAST_CHECK_OP(name, op, val1, val2)                                  \
    if (std::string* _result =                                          \
        ::fast::Check##name##Impl((val1), (val2),                    \
                                     #val1 " " #op " " #val2))          \
        ::fast::LogMessage(__FILE__, __LINE__, __func__, _result).stream()


#define CHECK_EQ(val1, val2) FAST_CHECK_OP(EQ, ==, val1, val2)
#define CHECK_NE(val1, val2) FAST_CHECK_OP(NE, !=, val1, val2)
#define CHECK_LE(val1, val2) FAST_CHECK_OP(LE, <=, val1, val2)
#define CHECK_LT(val1, val2) FAST_CHECK_OP(LT, < , val1, val2)
#define CHECK_GE(val1, val2) FAST_CHECK_OP(GE, >=, val1, val2)
#define CHECK_GT(val1, val2) FAST_CHECK_OP(GT, > , val1, val2)

#if defined(NDEBUG) && !defined(DCHECK_ALWAYS_ON)
#define DCHECK_IS_ON() 0
#else
#define DCHECK_IS_ON() 1
#endif


namespace fast {
#if DCHECK_IS_ON()
constexpr LogLevel DCHECK = FATAL;
#else
constexpr LogLevel DCHECK = INFO;
#endif
}  // namespace fast
// DCHECK et al. make sure to reference |condition| regardless of
// whether DCHECKs are enabled; this is so that we don't get unused
// variable warnings if the only use of a variable is in a DCHECK.
// This behavior is different from DLOG_IF et al.

#define DCHECK(condition)                                               \
    FAST_LAZY_STREAM(FAST_LOG_STREAM(DCHECK), DCHECK_IS_ON() && !(condition)) \
    << "Check failed: " #condition ". "

#define DPCHECK(condition)                                              \
    FAST_LAZY_STREAM(FAST_PLOG_STREAM(DCHECK), DCHECK_IS_ON() && !(condition)) \
    << "Check failed: " #condition ". "

// Helper macro for binary operators.
// Don't use this macro directly in your code, use DCHECK_EQ et al below.
#define FAST_DCHECK_OP(name, op, val1, val2)                           \
    if (DCHECK_IS_ON())                                                   \
        if (std::string* _result =                                      \
            ::fast::Check##name##Impl((val1), (val2),                \
                                         #val1 " " #op " " #val2))      \
            ::fast::LogMessage(                                      \
                __FILE__, __LINE__, __func__,                           \
                ::fast::DCHECK,                                 \
                _result).stream()

// Equality/Inequality checks - compare two values, and log a
// BLOG_DCHECK message including the two values when the result is not
// as expected.  The values must have operator<<(ostream, ...)
// defined.
//
// You may append to the error message like so:
//   DCHECK_NE(1, 2) << ": The world must be ending!";
//
// We are very careful to ensure that each argument is evaluated exactly
// once, and that anything which is legal to pass as a function argument is
// legal here.  In particular, the arguments may be temporary expressions
// which will end up being destroyed at the end of the apparent statement,
// for example:
//   DCHECK_EQ(string("abc")[1], 'b');
//
// WARNING: These may not compile correctly if one of the arguments is a pointer
// and the other is NULL. To work around this, simply static_cast NULL to the
// type of the desired pointer.

#define DCHECK_EQ(val1, val2) FAST_DCHECK_OP(EQ, ==, val1, val2)
#define DCHECK_NE(val1, val2) FAST_DCHECK_OP(NE, !=, val1, val2)
#define DCHECK_LE(val1, val2) FAST_DCHECK_OP(LE, <=, val1, val2)
#define DCHECK_LT(val1, val2) FAST_DCHECK_OP(LT, < , val1, val2)
#define DCHECK_GE(val1, val2) FAST_DCHECK_OP(GE, >=, val1, val2)
#define DCHECK_GT(val1, val2) FAST_DCHECK_OP(GT, > , val1, val2)
// Helper macro included by all *_EVERY_N macros.
#define FAST_LOG_IF_EVERY_N_IMPL(logifmacro, severity, condition, N)   \
    static ::fast::logging_detail::Atomic32 FAST_CONCAT(logeveryn_, __LINE__) = -1; \
    const static int FAST_CONCAT(logeveryn_sc_, __LINE__) = (N);       \
    const int FAST_CONCAT(logeveryn_c_, __LINE__) =                    \
        ::fast::logging_detail::NoBarrier_AtomicIncrement(&FAST_CONCAT(logeveryn_, __LINE__), 1); \
    logifmacro(severity, (condition) && FAST_CONCAT(logeveryn_c_, __LINE__) / \
               FAST_CONCAT(logeveryn_sc_, __LINE__) * FAST_CONCAT(logeveryn_sc_, __LINE__) \
               == FAST_CONCAT(logeveryn_c_, __LINE__))

// Helper macro included by all *_FIRST_N macros.
#define FAST_LOG_IF_FIRST_N_IMPL(logifmacro, severity, condition, N)   \
    static ::fast::logging_detail::Atomic32 FAST_CONCAT(logfstn_, __LINE__) = 0; \
    logifmacro(severity, (condition) && FAST_CONCAT(logfstn_, __LINE__).load(std::memory_order_relaxed) < N && \
               ::fast::logging_detail::NoBarrier_AtomicIncrement(&FAST_CONCAT(logfstn_, __LINE__), 1) <= N)

// Helper macro included by all *_EVERY_SECOND macros.
#define FAST_LOG_IF_EVERY_SECOND_IMPL(logifmacro, severity, condition) \
    static ::fast::logging_detail::Atomic64 FAST_CONCAT(logeverys_, __LINE__) = 0; \
    const int64_t FAST_CONCAT(logeverys_ts_, __LINE__) = ::fast::logging_detail::gettimeofday_us(); \
    const int64_t FAST_CONCAT(logeverys_seen_, __LINE__) = FAST_CONCAT(logeverys_, __LINE__).load(std::memory_order_relaxed); \
    logifmacro(severity, (condition) && FAST_CONCAT(logeverys_ts_, __LINE__) >= \
               (FAST_CONCAT(logeverys_seen_, __LINE__) + 1000000L) &&  \
               ::fast::logging_detail::NoBarrier_CompareAndSwap(                \
                   &FAST_CONCAT(logeverys_, __LINE__),                 \
                   FAST_CONCAT(logeverys_seen_, __LINE__),             \
                   FAST_CONCAT(logeverys_ts_, __LINE__))               \
               == FAST_CONCAT(logeverys_seen_, __LINE__))


#define LOG_ONCE(severity) LOG_FIRST_N(severity, 1)
#define LOG_IF_ONCE(severity, condition) LOG_IF_FIRST_N(severity, condition, 1)
#define LOG_EVERY_N(severity, N) FAST_LOG_IF_EVERY_N_IMPL(LOG_IF, severity, true, N)
#define LOG_IF_EVERY_N(severity, condition, N) FAST_LOG_IF_EVERY_N_IMPL(LOG_IF, severity, condition, N)
#define LOG_FIRST_N(severity, N) FAST_LOG_IF_FIRST_N_IMPL(LOG_IF, severity, true, N)
#define LOG_IF_FIRST_N(severity, condition, N) FAST_LOG_IF_FIRST_N_IMPL(LOG_IF, severity, condition, N)
#define LOG_EVERY_SECOND(severity) FAST_LOG_IF_EVERY_SECOND_IMPL(LOG_IF, severity, true)
#define LOG_IF_EVERY_SECOND(severity, condition) FAST_LOG_IF_EVERY_SECOND_IMPL(LOG_IF, severity, condition)
#define PLOG_ONCE(severity) PLOG_FIRST_N(severity, 1)
#define PLOG_IF_ONCE(severity, condition) PLOG_IF_FIRST_N(severity, condition, 1)
#define PLOG_EVERY_N(severity, N) FAST_LOG_IF_EVERY_N_IMPL(PLOG_IF, severity, true, N)
#define PLOG_IF_EVERY_N(severity, condition, N) FAST_LOG_IF_EVERY_N_IMPL(PLOG_IF, severity, condition, N)
#define PLOG_FIRST_N(severity, N) FAST_LOG_IF_FIRST_N_IMPL(PLOG_IF, severity, true, N)
#define PLOG_IF_FIRST_N(severity, condition, N) FAST_LOG_IF_FIRST_N_IMPL(PLOG_IF, severity, condition, N)
#define PLOG_EVERY_SECOND(severity) FAST_LOG_IF_EVERY_SECOND_IMPL(PLOG_IF, severity, true)
#define PLOG_IF_EVERY_SECOND(severity, condition) FAST_LOG_IF_EVERY_SECOND_IMPL(PLOG_IF, severity, condition)
