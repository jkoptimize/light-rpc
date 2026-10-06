#include <gtest/gtest.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>
#include <time.h>
#include <vector>

#include "bthread.h"    // bthread_* C API
#include "types.h"      // bthread_t, bthread_mutex_t, bthread_sem_t, bthread_rwlock_t
#include "mutex.h"
#include "butil/time.h"

namespace fast {
namespace {

// --- 调度器冒烟测试 ---

void* set_flag(void* arg) {
    static_cast<std::atomic<bool>*>(arg)->store(true);
    return nullptr;
}

TEST(BthreadBasicTest, StartAndJoin) {
    std::atomic<bool> flag{false};
    bthread_t tid;
    ASSERT_EQ(0, bthread_start_background(&tid, nullptr, set_flag, &flag));
    ASSERT_NE(INVALID_BTHREAD, tid);
    ASSERT_EQ(0, bthread_join(tid, nullptr));
    EXPECT_TRUE(flag.load());
}

TEST(BthreadBasicTest, SelfIsZeroInPthread) {
    EXPECT_EQ(INVALID_BTHREAD, bthread_self());
}

// --- mutex ---

TEST(BthreadMutexTest, MutualExclusion) {
    bthread_mutex_t mutex;
    ASSERT_EQ(0, bthread_mutex_init(&mutex, nullptr));
    int counter = 0;
    const int N = 10000;
    std::thread t1([&]() {
        for (int i = 0; i < N; ++i) {
            bthread_mutex_lock(&mutex);
            ++counter;
            bthread_mutex_unlock(&mutex);
        }
    });
    std::thread t2([&]() {
        for (int i = 0; i < N; ++i) {
            bthread_mutex_lock(&mutex);
            ++counter;
            bthread_mutex_unlock(&mutex);
        }
    });
    t1.join();
    t2.join();
    EXPECT_EQ(2 * N, counter);
    ASSERT_EQ(0, bthread_mutex_destroy(&mutex));
}

TEST(BthreadMutexTest, Trylock) {
    bthread_mutex_t mutex;
    ASSERT_EQ(0, bthread_mutex_init(&mutex, nullptr));
    ASSERT_EQ(0, bthread_mutex_lock(&mutex));
    EXPECT_EQ(EBUSY, bthread_mutex_trylock(&mutex));
    ASSERT_EQ(0, bthread_mutex_unlock(&mutex));
    ASSERT_EQ(0, bthread_mutex_destroy(&mutex));
}

TEST(BthreadMutexTest, Timedlock) {
    bthread_mutex_t mutex;
    ASSERT_EQ(0, bthread_mutex_init(&mutex, nullptr));
    ASSERT_EQ(0, bthread_mutex_lock(&mutex));
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_nsec += 10 * 1000 * 1000;  // +10ms
    if (abstime.tv_nsec >= 1000000000) {
        ++abstime.tv_sec;
        abstime.tv_nsec -= 1000000000;
    }
    EXPECT_EQ(ETIMEDOUT, bthread_mutex_timedlock(&mutex, &abstime));
    ASSERT_EQ(0, bthread_mutex_unlock(&mutex));
    ASSERT_EQ(0, bthread_mutex_destroy(&mutex));
}

// --- FastPthreadMutex ---

TEST(FastPthreadMutexTest, TryLockAndExpiredDeadlineWhenFree) {
    FastPthreadMutex mutex;
    ASSERT_TRUE(mutex.try_lock());
    std::thread contender([&]() { EXPECT_FALSE(mutex.try_lock()); });
    contender.join();
    mutex.unlock();
    const timespec expired = {0, 0};
    // Like brpc, the immediate acquisition precedes the deadline check.
    ASSERT_TRUE(mutex.timed_lock(&expired));
    mutex.unlock();
}

TEST(FastPthreadMutexTest, ContendedCounter) {
    FastPthreadMutex mutex;
    int counter = 0;
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int j = 0; j < 10000; ++j) {
                std::lock_guard<FastPthreadMutex> guard(mutex);
                ++counter;
            }
        });
    }
    start.store(true);
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(80000, counter);
}

TEST(FastPthreadMutexTest, TimedWaitExpiresAndMutexRemainsUsable) {
    FastPthreadMutex mutex;
    mutex.lock();
    std::thread contender([&]() {
        const timespec deadline = butil::microseconds_to_timespec(
            butil::gettimeofday_us() + 50000);
        const bool acquired = mutex.timed_lock(&deadline);
        const int error = errno;
        EXPECT_FALSE(acquired);
        if (acquired) mutex.unlock();
        else EXPECT_EQ(ETIMEDOUT, error);
    });
    contender.join();
    mutex.unlock();
    ASSERT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST(FastPthreadMutexTest, UnlockReleasesTimedWaiter) {
    FastPthreadMutex mutex;
    mutex.lock();
    std::atomic<bool> entering{false};
    int value = 0;
    std::thread contender([&]() {
        const timespec deadline = butil::microseconds_to_timespec(
            butil::gettimeofday_us() + 5000000);
        entering.store(true);
        const bool acquired = mutex.timed_lock(&deadline);
        EXPECT_TRUE(acquired);
        if (acquired) {
            EXPECT_EQ(42, value);
            mutex.unlock();
        }
    });
    while (!entering.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    value = 42;
    mutex.unlock();
    contender.join();
}

// --- semaphore ---

TEST(BthreadSemTest, TrywaitWhenZero) {
    bthread_sem_t sem;
    ASSERT_EQ(0, bthread_sem_init(&sem, 0));
    EXPECT_EQ(EAGAIN, bthread_sem_trywait(&sem));
    ASSERT_EQ(0, bthread_sem_destroy(&sem));
}

TEST(BthreadSemTest, WaitUnblocksOnPost) {
    bthread_sem_t sem;
    ASSERT_EQ(0, bthread_sem_init(&sem, 0));
    std::atomic<bool> done{false};
    std::thread t([&]() {
        EXPECT_EQ(0, bthread_sem_wait(&sem));
        done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(done.load());
    ASSERT_EQ(0, bthread_sem_post(&sem));
    t.join();
    EXPECT_TRUE(done.load());
    ASSERT_EQ(0, bthread_sem_destroy(&sem));
}

TEST(BthreadSemTest, Timedwait) {
    bthread_sem_t sem;
    ASSERT_EQ(0, bthread_sem_init(&sem, 0));
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_nsec += 10 * 1000 * 1000;  // +10ms
    if (abstime.tv_nsec >= 1000000000) {
        ++abstime.tv_sec;
        abstime.tv_nsec -= 1000000000;
    }
    EXPECT_EQ(ETIMEDOUT, bthread_sem_timedwait(&sem, &abstime));
    ASSERT_EQ(0, bthread_sem_destroy(&sem));
}

TEST(BthreadSemTest, PostN) {
    bthread_sem_t sem;
    ASSERT_EQ(0, bthread_sem_init(&sem, 0));
    ASSERT_EQ(0, bthread_sem_post_n(&sem, 3));
    EXPECT_EQ(0, bthread_sem_trywait(&sem));
    EXPECT_EQ(0, bthread_sem_trywait(&sem));
    EXPECT_EQ(0, bthread_sem_trywait(&sem));
    EXPECT_EQ(EAGAIN, bthread_sem_trywait(&sem));
    ASSERT_EQ(0, bthread_sem_destroy(&sem));
}

// --- rwlock ---

TEST(BthreadRwlockTest, WrlockExcludesRdlock) {
    bthread_rwlock_t rwlock;
    ASSERT_EQ(0, bthread_rwlock_init(&rwlock, nullptr));
    ASSERT_EQ(0, bthread_rwlock_wrlock(&rwlock));
    EXPECT_EQ(EBUSY, bthread_rwlock_tryrdlock(&rwlock));
    ASSERT_EQ(0, bthread_rwlock_unlock(&rwlock));

    ASSERT_EQ(0, bthread_rwlock_rdlock(&rwlock));
    ASSERT_EQ(0, bthread_rwlock_unlock(&rwlock));
    ASSERT_EQ(0, bthread_rwlock_destroy(&rwlock));
}

TEST(BthreadRwlockTest, MultipleReaders) {
    bthread_rwlock_t rwlock;
    ASSERT_EQ(0, bthread_rwlock_init(&rwlock, nullptr));
    ASSERT_EQ(0, bthread_rwlock_rdlock(&rwlock));
    ASSERT_EQ(0, bthread_rwlock_rdlock(&rwlock));  // 第二读锁可同时持有
    ASSERT_EQ(0, bthread_rwlock_unlock(&rwlock));
    ASSERT_EQ(0, bthread_rwlock_unlock(&rwlock));
    ASSERT_EQ(0, bthread_rwlock_destroy(&rwlock));
}

TEST(BthreadRwlockTest, TrywrlockExcludesReaders) {
    bthread_rwlock_t rwlock;
    ASSERT_EQ(0, bthread_rwlock_init(&rwlock, nullptr));
    ASSERT_EQ(0, bthread_rwlock_rdlock(&rwlock));
    EXPECT_EQ(EBUSY, bthread_rwlock_trywrlock(&rwlock));
    ASSERT_EQ(0, bthread_rwlock_unlock(&rwlock));
    ASSERT_EQ(0, bthread_rwlock_destroy(&rwlock));
}

}  // namespace
}  // namespace fast
