#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "inc/fast_log.h"

namespace {
// Linker wrapping keeps time control out of the production logging code.
std::atomic<int64_t> fake_time_us{-1};
}

extern "C" int __real_gettimeofday(timeval*, void*);
extern "C" int __wrap_gettimeofday(timeval* tv, void* tz) {
    const int64_t now = fake_time_us.load(std::memory_order_relaxed);
    if (now < 0) {
        return __real_gettimeofday(tv, tz);
    }
    tv->tv_sec = now / 1000000;
    tv->tv_usec = now % 1000000;
    return 0;
}

namespace {
template <typename Fn>
void RunTogether(Fn fn) {
    std::mutex mutex;
    std::condition_variable cv;
    int ready = 0;
    bool start = false;
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++ready;
                cv.notify_all();
                cv.wait(lock, [&] { return start; });
            }
            fn();
        });
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return ready == 16; });
        start = true;
    }
    cv.notify_all();
    for (auto& thread : threads) { thread.join(); }
}

class FastLogTest : public testing::Test {
protected:
    void SetUp() override {
        saved_level_ = fast::GetMinLogLevel();
        fast::SetMinLogLevel(fast::INFO);
    }
    void TearDown() override {
        fast::SetMinLogLevel(saved_level_);
        fake_time_us.store(-1);
    }
    int saved_level_;
};

TEST_F(FastLogTest, FalseConditionSkipsArgumentsAndOutput) {
    int conditions = 0;
    int arguments = 0;
    testing::internal::CaptureStderr();
    LOG_IF(ERROR, (++conditions, false)) << ++arguments;
    EXPECT_EQ("", testing::internal::GetCapturedStderr());
    EXPECT_EQ(1, conditions);
    EXPECT_EQ(0, arguments);
}

TEST_F(FastLogTest, DisabledLevelSkipsConditionAndArguments) {
    fast::SetMinLogLevel(fast::ERROR);
    int conditions = 0;
    int arguments = 0;
    testing::internal::CaptureStderr();
    LOG(INFO) << ++arguments;
    LOG_IF(INFO, ++conditions) << ++arguments;
    PLOG_IF(INFO, ++conditions) << ++arguments;
    EXPECT_EQ("", testing::internal::GetCapturedStderr());
    EXPECT_EQ(0, conditions);
    EXPECT_EQ(0, arguments);
}

TEST_F(FastLogTest, EnabledLogSupportsManipulatorsAndOuterElse) {
    testing::internal::CaptureStderr();
    if (true)
        LOG_IF(INFO, true) << std::hex << 255 << std::endl;
    else
        ADD_FAILURE();
    EXPECT_NE(std::string::npos,
              testing::internal::GetCapturedStderr().find("ff"));
}

TEST_F(FastLogTest, CheckAlwaysEvaluatesOnceAndSkipsSuccessMessage) {
    int calls = 0;
    int messages = 0;
    CHECK(++calls == 1) << ++messages;
    CHECK_EQ(++calls, 2) << ++messages;
    CHECK_NE(calls, 0);
    CHECK_LT(calls, 3);
    CHECK_LE(calls, 2);
    CHECK_GT(calls, 1);
    CHECK_GE(calls, 2);
    EXPECT_EQ(2, calls);
    EXPECT_EQ(0, messages);
}

TEST_F(FastLogTest, DcheckHonorsBuildModeAndForceFlag) {
    int calls = 0;
    int messages = 0;
    DCHECK(++calls == 1) << ++messages;
    DCHECK_EQ(++calls, 2) << ++messages;
#if defined(NDEBUG) && !defined(DCHECK_ALWAYS_ON)
    EXPECT_EQ(0, calls);
#else
    EXPECT_EQ(2, calls);
#endif
    EXPECT_EQ(0, messages);
}

TEST_F(FastLogTest, FatalAndFailedChecksAbort) {
    fast::SetMinLogLevel(100);  // FATAL must remain enabled.
    EXPECT_EXIT({ LOG(FATAL) << "fatal marker"; },
                testing::KilledBySignal(SIGABRT), "fatal marker");
    EXPECT_EXIT({ CHECK(false) << "check marker"; },
                testing::KilledBySignal(SIGABRT), "check marker");
    EXPECT_EXIT({ CHECK_EQ(1, 2) << "binary marker"; },
                testing::KilledBySignal(SIGABRT), "1 vs 2.*binary marker");
#if !defined(NDEBUG) || defined(DCHECK_ALWAYS_ON)
    EXPECT_EXIT({ DCHECK(false) << "debug marker"; },
                testing::KilledBySignal(SIGABRT), "debug marker");
#endif
}

TEST_F(FastLogTest, PlogCapturesErrnoBeforeStreamArguments) {
    testing::internal::CaptureStderr();
    errno = ENOENT;
    PLOG(ERROR) << "plog marker " << ([] { errno = EINVAL; return 42; })();
    const std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(std::string::npos, output.find("plog marker 42"));
    EXPECT_NE(std::string::npos, output.find(strerror(ENOENT)));
}

TEST_F(FastLogTest, EverySecondPreservesBoundaryAndClockRollback) {
    int arguments = 0;
    auto log = [&] { LOG_EVERY_SECOND(INFO) << ++arguments; };
    testing::internal::CaptureStderr();
    fake_time_us = 10000000;
    log();
    log();
    fake_time_us = 10999999;
    log();
    EXPECT_EQ(1, arguments);
    fake_time_us = 11000000;
    log();
    EXPECT_EQ(2, arguments);
    fake_time_us = 9000000;
    log();
    EXPECT_EQ(2, arguments);
    fake_time_us = 12000000;
    log();
    EXPECT_EQ(3, arguments);
    testing::internal::GetCapturedStderr();
}

TEST_F(FastLogTest, FilteredLogsDoNotConsumeInterval) {
    int arguments = 0;
    auto log = [&](bool condition) {
        LOG_IF_EVERY_SECOND(INFO, condition) << ++arguments;
    };
    testing::internal::CaptureStderr();
    fake_time_us = 20000000;
    log(false);
    fast::SetMinLogLevel(fast::ERROR);
    log(true);
    fast::SetMinLogLevel(fast::INFO);
    log(true);
    log(true);
    EXPECT_EQ(1, arguments);
    testing::internal::GetCapturedStderr();
}

TEST_F(FastLogTest, RateLimitIsSharedAcrossThreadsAndSeparateAcrossSites) {
    fake_time_us = 30000000;
    std::atomic<int> arguments{0};
    auto log = [&] { LOG_EVERY_SECOND(INFO) << ++arguments; };
    testing::internal::CaptureStderr();
    RunTogether([&] {
        for (int j = 0; j < 100; ++j) { log(); }
    });
    EXPECT_EQ(1, arguments.load());
    LOG_EVERY_SECOND(INFO) << ++arguments;
    EXPECT_EQ(2, arguments.load());
    testing::internal::GetCapturedStderr();
}

TEST_F(FastLogTest, OnceIsSharedAcrossThreads) {
    std::atomic<int> arguments{0};
    auto log = [&] { LOG_ONCE(INFO) << ++arguments; };
    testing::internal::CaptureStderr();
    RunTogether(log);
    EXPECT_EQ(1, arguments.load());
    testing::internal::GetCapturedStderr();
}

TEST_F(FastLogTest, PlogEverySecondSkipsSuppressedArguments) {
    fake_time_us = 40000000;
    int arguments = 0;
    testing::internal::CaptureStderr();
    for (int i = 0; i < 5; ++i) {
        errno = EINVAL;
        PLOG_EVERY_SECOND(ERROR) << ++arguments;
    }
    EXPECT_EQ(1, arguments);
    EXPECT_NE(std::string::npos,
              testing::internal::GetCapturedStderr().find(strerror(EINVAL)));
}

TEST_F(FastLogTest, EveryNCountsCallsAndFirstNCountsAcceptedConditions) {
    int periodic = 0;
    int first = 0;
    testing::internal::CaptureStderr();
    for (int i = 0; i < 10; ++i) {
        LOG_EVERY_N(INFO, 3) << ++periodic;
        LOG_IF_FIRST_N(INFO, i % 2 == 0, 2) << ++first;
    }
    EXPECT_EQ(4, periodic);
    EXPECT_EQ(2, first);
    testing::internal::GetCapturedStderr();
}
}  // namespace
