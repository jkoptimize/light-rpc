#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <thread>
#include <unistd.h>

#include "inc/fast_bthread_config.h"
#include "bthread.h"
#include "butex.h"
#include "errno.h"
#include "id.h"
#include "timer_thread.h"
#include "butil/time.h"

// Implemented by bthread.cpp; brpc exposes this through unstable.h.
extern "C" void bthread_stop_world();

namespace fast {
namespace {

void* Noop(void*) { return nullptr; }

// No scheduler is created in the parent until all death tests have finished.
TEST(BthreadStartupDeathTest, RejectsInvalidParkingLotCount) {
    for (int count : {0, BTHREAD_MIN_PARKINGLOT - 1, BTHREAD_MAX_PARKINGLOT + 1}) {
        ASSERT_EXIT({
            alarm(3);
            FastBthreadConfig::Get().parking_lot_of_each_tag = count;
            bthread_t tid;
            const int rc = bthread_start_background(&tid, nullptr, Noop, nullptr);
            _exit(rc != 0 ? 0 : 1);
        }, ::testing::ExitedWithCode(0), "");
    }
}

TEST(BthreadStartupDeathTest, RejectsInvalidConcurrency) {
    for (int count : {BTHREAD_MIN_CONCURRENCY - 1, BTHREAD_MAX_CONCURRENCY + 1}) {
        ASSERT_EXIT({
            alarm(3);
            FastBthreadConfig::Get().bthread_concurrency = count;
            bthread_t tid;
            const int rc = bthread_start_background(&tid, nullptr, Noop, nullptr);
            _exit(rc != 0 ? 0 : 1);
        }, ::testing::ExitedWithCode(0), "");
    }
}

TEST(BthreadStartupDeathTest, RejectsInvalidMinimumConcurrency) {
    for (int count : {BTHREAD_MIN_CONCURRENCY - 1, 10}) {
        ASSERT_EXIT({
            alarm(3);
            FastBthreadConfig::Get().bthread_min_concurrency = count;
            bthread_t tid;
            const int rc = bthread_start_background(&tid, nullptr, Noop, nullptr);
            _exit(rc != 0 ? 0 : 1);
        }, ::testing::ExitedWithCode(0), "");
    }
}

TEST(BthreadStartupDeathTest, TaggedSetterHandlesRejectedStartupAndRecovery) {
    ASSERT_EXIT({
        alarm(3);
        FastBthreadConfig::Get().parking_lot_of_each_tag = 0;
        if (bthread_setconcurrency_by_tag(4, 0) == 0) _exit(1);
        FastBthreadConfig::Get().parking_lot_of_each_tag = 4;
        bthread_t tid;
        if (bthread_start_background(&tid, nullptr, Noop, nullptr) != 0) _exit(2);
        if (bthread_join(tid, nullptr) != 0) _exit(3);
        bthread_stop_world();
        _exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

TEST(BthreadStartupDeathTest, ValidLazyStartupWithTwoTagsAndShutdown) {
    ASSERT_EXIT({
        alarm(5);
        auto& config = FastBthreadConfig::Get();
        config.bthread_min_concurrency = 4;
        config.task_group_ntags = 2;
        config.enable_bthread_priority_queue = true;
        config.parking_lot_no_signal_when_no_waiter = true;
        std::atomic<int> seen{0};
        for (int tag = 0; tag < 2; ++tag) {
            bthread_attr_t attr = BTHREAD_ATTR_NORMAL;
            attr.tag = tag;
            bthread_t tid;
            if (bthread_start_background(&tid, &attr, [](void* ptr) -> void* {
                static_cast<std::atomic<int>*>(ptr)->fetch_or(1 << bthread_self_tag());
                return nullptr;
            }, &seen) != 0) _exit(1);
            if (bthread_join(tid, nullptr) != 0) _exit(2);
        }
        if (seen.load() != 3) _exit(3);
        bthread_stop_world();
        _exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

std::atomic<int> once_calls{0};
void InitOnce() { once_calls.fetch_add(1); }

TEST(BthreadRuntime, OnceTlsAndErrnoSurviveYieldAndSleep) {
    struct State {
        bthread_once_t once;
        bthread_key_t key;
        std::atomic<int> destructed{0};
    } state;
    struct Local { State* state; int marker; } locals[32];
    ASSERT_EQ(0, bthread_key_create(&state.key, [](void* ptr) {
        static_cast<Local*>(ptr)->state->destructed.fetch_add(1);
    }));
    bthread_t tids[32];
    for (int i = 0; i < 32; ++i) {
        locals[i] = {&state, 100 + i};
        ASSERT_EQ(0, bthread_start_background(&tids[i], nullptr, [](void* ptr) -> void* {
            auto& local = *static_cast<Local*>(ptr);
            EXPECT_EQ(0, bthread_once(&local.state->once, InitOnce));
            EXPECT_EQ(0, bthread_setspecific(local.state->key, &local));
            for (int j = 0; j < 20; ++j) {
                errno = local.marker;
                bthread_yield();
                EXPECT_EQ(local.marker, errno);
                EXPECT_EQ(&local, bthread_getspecific(local.state->key));
            }
            EXPECT_EQ(0, bthread_usleep(1000));
            EXPECT_EQ(&local, bthread_getspecific(local.state->key));
            return nullptr;
        }, &locals[i]));
    }
    for (auto tid : tids) ASSERT_EQ(0, bthread_join(tid, nullptr));
    EXPECT_EQ(1, once_calls.load());
    EXPECT_EQ(32, state.destructed.load());
    EXPECT_EQ(0, bthread_key_delete(state.key));
}

TEST(BthreadRuntime, ConditionBroadcastRequeuesAndReacquiresMutex) {
    struct State {
        bthread_mutex_t mutex;
        bthread_cond_t cond;
        int waiting = 0;
        bool ready = false;
        int completed = 0;
    } state;
    ASSERT_EQ(0, bthread_mutex_init(&state.mutex, nullptr));
    ASSERT_EQ(0, bthread_cond_init(&state.cond, nullptr));
    bthread_t tids[8];
    for (auto& tid : tids) {
        ASSERT_EQ(0, bthread_start_background(&tid, nullptr, [](void* ptr) -> void* {
            auto& s = *static_cast<State*>(ptr);
            bthread_mutex_lock(&s.mutex);
            ++s.waiting;
            const auto deadline = butil::seconds_from_now(3);
            while (!s.ready) {
                const int rc = bthread_cond_timedwait(&s.cond, &s.mutex, &deadline);
                EXPECT_EQ(0, rc);
                if (rc != 0) break;
            }
            EXPECT_TRUE(s.ready);
            ++s.completed;
            bthread_mutex_unlock(&s.mutex);
            return nullptr;
        }, &state));
    }
    const int64_t deadline = butil::monotonic_time_ms() + 2000;
    bool enrolled = false;
    while (butil::monotonic_time_ms() < deadline) {
        bthread_mutex_lock(&state.mutex);
        if (state.waiting == 8) {
            enrolled = true;
            state.ready = true;
            bthread_cond_broadcast(&state.cond);
        }
        bthread_mutex_unlock(&state.mutex);
        if (enrolled) break;
        usleep(1000);
    }
    EXPECT_TRUE(enrolled);
    for (auto tid : tids) ASSERT_EQ(0, bthread_join(tid, nullptr));
    EXPECT_EQ(8, state.completed);
    EXPECT_EQ(0, bthread_cond_destroy(&state.cond));
    EXPECT_EQ(0, bthread_mutex_destroy(&state.mutex));
}

TEST(BthreadRuntime, InterruptResumesButexWaiter) {
    auto* value = butex_create_checked<std::atomic<int>>();
    ASSERT_NE(nullptr, value);
    value->store(0);
    struct State {
        std::atomic<int>* value;
        std::atomic<bool> entered{false};
        int result = 0;
        int error = 0;
    } state{value};
    bthread_t tid;
    ASSERT_EQ(0, bthread_start_background(&tid, nullptr, [](void* ptr) -> void* {
        auto& s = *static_cast<State*>(ptr);
        const auto deadline = butil::seconds_from_now(3);
        s.entered.store(true);
        s.result = butex_wait(s.value, 0, &deadline);
        s.error = errno;
        return nullptr;
    }, &state));
    while (!state.entered.load()) sched_yield();
    EXPECT_EQ(0, bthread_interrupt(tid));
    ASSERT_EQ(0, bthread_join(tid, nullptr));
    EXPECT_EQ(-1, state.result);
    EXPECT_EQ(EINTR, state.error);
    butex_destroy(value);
}

TEST(BthreadRuntime, IdDefersErrorUntilUnlockAndRejectsDestroyedId) {
    std::atomic<int> error{0};
    bthread_id_t id;
    ASSERT_EQ(0, bthread_id_create(&id, &error,
        [](bthread_id_t id, void* ptr, int ec) {
            static_cast<std::atomic<int>*>(ptr)->store(ec);
            return bthread_id_unlock_and_destroy(id);
        }));
    void* data = nullptr;
    ASSERT_EQ(0, bthread_id_lock(id, &data));
    EXPECT_EQ(&error, data);
    std::thread notifier([&] { EXPECT_EQ(0, bthread_id_error(id, EPIPE)); });
    notifier.join();
    EXPECT_EQ(0, error.load());
    EXPECT_EQ(0, bthread_id_unlock(id));
    EXPECT_EQ(EPIPE, error.load());
    EXPECT_EQ(0, bthread_id_join(id));
    EXPECT_EQ(EINVAL, bthread_id_trylock(id, &data));
}

TEST(BthreadRuntime, TimerCancelAndExecute) {
    TimerThread timer;
    ASSERT_EQ(0, timer.start(nullptr));
    std::atomic<int> cancelled_calls{0};
    const auto cancelled = timer.schedule([](void* ptr) {
        static_cast<std::atomic<int>*>(ptr)->fetch_add(1);
    }, &cancelled_calls, butil::seconds_from_now(60));
    ASSERT_NE(TimerThread::INVALID_TASK_ID, cancelled);
    EXPECT_EQ(0, timer.unschedule(cancelled));
    std::promise<void> fired;
    auto future = fired.get_future();
    ASSERT_NE(TimerThread::INVALID_TASK_ID, timer.schedule([](void* ptr) {
        static_cast<std::promise<void>*>(ptr)->set_value();
    }, &fired, butil::milliseconds_from_now(10)));
    EXPECT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds(2)));
    timer.stop_and_join();
    EXPECT_EQ(0, cancelled_calls.load());
}

} // namespace
} // namespace fast

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
