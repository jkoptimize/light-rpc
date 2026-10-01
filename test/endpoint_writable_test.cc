#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include "bthread.h"
#include "butex.h"
#include "butil/time.h"
#include "test/endpoint_test_owner.h"

namespace {
using namespace std::chrono_literals;
struct WaitGate {
    std::promise<void> entered;
    std::promise<void> resume;
    int result = 0;
    int error = 0;
};
thread_local WaitGate* wait_gate = nullptr;
}

extern "C" int real_butex_wait(void*, int, const timespec*, bool)
    asm("__real__ZN4fast10butex_waitEPviPK8timespecb");
extern "C" int wrapped_butex_wait(void*, int, const timespec*, bool)
    asm("__wrap__ZN4fast10butex_waitEPviPK8timespecb");
extern "C" int wrapped_butex_wait(void* b, int expected, const timespec* deadline,
                                  bool prepend) {
    WaitGate* gate = wait_gate;
    if (gate) {
        gate->entered.set_value();
        gate->resume.get_future().wait();
    }
    const int rc = real_butex_wait(b, expected, deadline, prepend);
    if (gate) {
        gate->result = rc;
        gate->error = rc < 0 ? errno : 0;
    }
    return rc;
}

namespace fast {
// For unit tests only: no QP/CQ is allocated; SEND WC handling invokes no verbs.
class FastRdmaEndpointWritableTestPeer {
public:
    static void Exhaust(FastRdmaEndpoint& ep, int remote_window = 125) {
        ep.SetNegotiatedParams(128, 128, 128, 128, 8192);
        ep.sq_window_size_.store(0, std::memory_order_relaxed);
        ep.remote_rq_window_size_.store(remote_window, std::memory_order_relaxed);
    }
    static int Generation(FastRdmaEndpoint& ep) {
        return ep.writable_butex_->load(std::memory_order_relaxed);
    }
};

namespace {
using Peer = FastRdmaEndpointWritableTestPeer;
std::future<void> StartPausedWait(FastRdmaEndpoint& ep, WaitGate& gate) {
    return std::async(std::launch::async, [&ep, &gate] {
        wait_gate = &gate;
        ep.WaitForWritable();
        wait_gate = nullptr;
    });
}
void CompleteOne(FastRdmaEndpoint& ep) {
    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id = 1;
    EXPECT_EQ(0, ep.HandleCompletion(wc));
}

TEST(RdmaWritable, WindowRestoredBetweenPredicateAndButexWait) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    Peer::Exhaust(ep);
    WaitGate gate;
    auto worker = StartPausedWait(ep, gate);
    gate.entered.get_future().wait();
    CompleteOne(ep);
    gate.resume.set_value();
    worker.get();
    EXPECT_EQ(-1, gate.result);
    EXPECT_EQ(EWOULDBLOCK, gate.error);
    EXPECT_TRUE(ep.IsWritable());
    EXPECT_FALSE(ep.Failed());
}

TEST(RdmaWritable, FailureBetweenPredicateAndButexWait) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    Peer::Exhaust(ep);
    WaitGate gate;
    auto worker = StartPausedWait(ep, gate);
    gate.entered.get_future().wait();
    ep.SetFailed(EPIPE);
    gate.resume.set_value();
    worker.get();
    EXPECT_EQ(-1, gate.result);
    EXPECT_EQ(EWOULDBLOCK, gate.error);
    EXPECT_EQ(EPIPE, ep.error());
}

TEST(RdmaWritable, BelowThresholdRestorationReturnsForRetryAtDeadline) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    Peer::Exhaust(ep, 1); // writable after SEND, but below the wake threshold.
    const int generation = Peer::Generation(ep);
    WaitGate gate;
    auto worker = StartPausedWait(ep, gate);
    gate.entered.get_future().wait();
    CompleteOne(ep);
    EXPECT_TRUE(ep.IsWritable());
    EXPECT_EQ(generation, Peer::Generation(ep));
    gate.resume.set_value();
    worker.get();
    EXPECT_EQ(-1, gate.result);
    EXPECT_EQ(ETIMEDOUT, gate.error);
    EXPECT_FALSE(ep.Failed());
}

TEST(RdmaWritable, FailureBeforeWaitDoesNotWaitForTimeout) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    Peer::Exhaust(ep);
    ep.SetFailed(EPIPE);
    // The wrapper would block if WaitForWritable entered butex_wait.
    WaitGate gate;
    auto worker = StartPausedWait(ep, gate);
    auto entered = gate.entered.get_future();
    const auto status = worker.wait_for(1s);
    gate.resume.set_value(); // allow cleanup even if the regression occurs.
    worker.get();
    EXPECT_EQ(std::future_status::ready, status);
    EXPECT_EQ(std::future_status::timeout, entered.wait_for(0s));
}

class ButexOwner {
public:
    ButexOwner() : value(butex_create_checked<std::atomic<int>>()) {
        CHECK(value != nullptr);
        value->store(0, std::memory_order_relaxed);
    }
    ~ButexOwner() { butex_destroy(value); }
    std::atomic<int>* value;
};

TEST(ButexRuntime, RegisteredPthreadWaiterIsWoken) {
    ButexOwner b;
    auto worker = std::async(std::launch::async, [&] {
        const auto deadline = butil::milliseconds_from_now(2000);
        const int rc = butex_wait(b.value, 0, &deadline);
        return std::make_pair(rc, rc < 0 ? errno : 0);
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    int woken = 0;
    // A successful wake count is the enrollment acknowledgement; no sleep
    // is used to guess whether the waiter has entered the butex queue.
    while (!(woken = butex_wake_except(b.value, INVALID_BTHREAD)) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    EXPECT_EQ(1, woken);
    const auto result = worker.get();
    // Wake after enqueue but before futex sleep may return EWOULDBLOCK.
    EXPECT_TRUE(result == std::make_pair(0, 0) ||
                result == std::make_pair(-1, EWOULDBLOCK));
}

struct BthreadWait {
    std::atomic<int>* value;
    int timeout_ms;
    int result = 0;
    int error = 0;
};
void* WaitInBthread(void* ptr) {
    auto& args = *static_cast<BthreadWait*>(ptr);
    const auto deadline = butil::milliseconds_from_now(args.timeout_ms);
    args.result = butex_wait(args.value, 0, &deadline);
    args.error = args.result < 0 ? errno : 0;
    return nullptr;
}

TEST(ButexRuntime, RegisteredBthreadWaiterIsWokenAndJoined) {
    ButexOwner b;
    BthreadWait args{b.value, 2000};
    bthread_t tid;
    ASSERT_EQ(0, bthread_start_background(&tid, nullptr, WaitInBthread, &args));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    int woken = 0;
    while (!(woken = butex_wake_except(b.value, INVALID_BTHREAD)) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    EXPECT_EQ(1, woken);
    ASSERT_EQ(0, bthread_join(tid, nullptr));
    EXPECT_EQ(0, args.result);
}

TEST(ButexRuntime, BthreadTimeoutResumesViaTimer) {
    ButexOwner b;
    BthreadWait args{b.value, 15};
    bthread_t tid;
    ASSERT_EQ(0, bthread_start_background(&tid, nullptr, WaitInBthread, &args));
    ASSERT_EQ(0, bthread_join(tid, nullptr));
    EXPECT_EQ(-1, args.result);
    EXPECT_EQ(ETIMEDOUT, args.error);
}

TEST(ButexRuntime, ContendedBthreadMutexRetainsOwnershipAcrossYield) {
    struct State { bthread_mutex_t mutex; int count = 0; } state;
    ASSERT_EQ(0, bthread_mutex_init(&state.mutex, nullptr));
    bthread_t tids[4];
    for (auto& tid : tids) {
        ASSERT_EQ(0, bthread_start_background(&tid, nullptr, [](void* ptr) -> void* {
            auto& s = *static_cast<State*>(ptr);
            for (int i = 0; i < 100; ++i) {
                bthread_mutex_lock(&s.mutex);
                const int value = s.count;
                bthread_yield();
                s.count = value + 1;
                bthread_mutex_unlock(&s.mutex);
            }
            return nullptr;
        }, &state));
    }
    for (auto tid : tids) ASSERT_EQ(0, bthread_join(tid, nullptr));
    EXPECT_EQ(400, state.count);
    EXPECT_EQ(0, bthread_mutex_destroy(&state.mutex));
}
} // namespace
} // namespace fast

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
