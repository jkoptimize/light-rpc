#include "test/endpoint_test_owner.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "fast_rdma_endpoint.h"

namespace {
struct YieldBarrier {
    std::promise<void> entered;
    std::promise<void> resume;
    std::shared_future<void> resumed = resume.get_future().share();
};
thread_local YieldBarrier* yield_barrier = nullptr;
}

extern "C" int __real_sched_yield();
extern "C" int __wrap_sched_yield() {
    if (yield_barrier != nullptr) {
        YieldBarrier* barrier = yield_barrier;
        yield_barrier = nullptr;
        barrier->entered.set_value();
        barrier->resumed.wait();
    }
    return __real_sched_yield();
}

namespace fast {

// For unit tests only: invoke the actual queue protocol without QPs or CQs.
class FastRdmaEndpointWriteTestPeer {
public:
    using Request = FastRdmaEndpoint::WriteRequest;
    static Request* Publish(FastRdmaEndpoint& ep, Request* req) {
        return ep.PublishWriteRequest(req);
    }
    static Request* Enqueue(FastRdmaEndpoint& ep, Request* req) {
        Request* prev = Publish(ep, req);
        req->next = prev;
        return prev;
    }
    static bool Complete(FastRdmaEndpoint& ep, Request* tail, bool singular,
                         Request** new_tail = nullptr) {
        return ep.IsWriteComplete(tail, singular, new_tail);
    }
    static int Fail(FastRdmaEndpoint& ep, Request* req, int error) {
        return ep.FailWrite(req, error);
    }
    static int Error(FastRdmaEndpoint& ep) { return ep.WriteError(); }
    static Request* Head(FastRdmaEndpoint& ep) {
        return ep._write_head.load(std::memory_order_acquire);
    }
    static void SetPending(FastRdmaEndpoint& ep, Request* req) {
        ep._pending_keepwrite_req.store(req, std::memory_order_release);
    }
    static Request* Pending(FastRdmaEndpoint& ep) {
        return ep._pending_keepwrite_req.load(std::memory_order_acquire);
    }
    static void FailPending(FastRdmaEndpoint& ep, int error) {
        ep.FailPendingWrite(error);
    }
};

namespace {
using Peer = FastRdmaEndpointWriteTestPeer;
using Request = Peer::Request;

IOBuf TrackedData(std::atomic<int>& released) {
    IOBuf data;
    char* bytes = new char[1]{'x'};
    data.append_user_data_with_meta(bytes, 1, [&released](void* p) {
        released.fetch_add(1, std::memory_order_relaxed);
        delete[] static_cast<char*>(p);
    }, 0);
    return data;
}

Request* TrackedRequest(std::atomic<int>& released) {
    auto* req = new Request;
    IOBuf data = TrackedData(released);
    req->data.swap(data);
    return req;
}

TEST(RdmaWriteQueue, RetainsIncompleteDataAndReversesNewRequestsInFifoOrder) {
    EndpointTestOwner endpoint_owner;
    auto& ep = endpoint_owner.get();
    std::atomic<int> released{0};
    Request* a = TrackedRequest(released);
    Request* b = TrackedRequest(released);
    Request* c = TrackedRequest(released);
    EXPECT_EQ(nullptr, Peer::Enqueue(ep, a));
    EXPECT_FALSE(Peer::Complete(ep, a, true));
    EXPECT_EQ(a, Peer::Enqueue(ep, b));
    EXPECT_EQ(b, Peer::Enqueue(ep, c));
    Request* tail = nullptr;
    EXPECT_FALSE(Peer::Complete(ep, a, true, &tail));
    EXPECT_EQ(c, tail);
    EXPECT_EQ(b, a->next);
    EXPECT_EQ(c, b->next);
    EXPECT_EQ(nullptr, c->next);
    EXPECT_FALSE(Peer::Complete(ep, c, false));
    EXPECT_EQ(c, Peer::Head(ep));
    EXPECT_EQ(-1, Peer::Fail(ep, a, EPIPE));
    EXPECT_EQ(3, released.load());
    EXPECT_EQ(nullptr, Peer::Head(ep));
}

TEST(RdmaWriteQueue, CompletedEpochReleasesTheWriteRight) {
    EndpointTestOwner endpoint_owner;
    auto& ep = endpoint_owner.get();
    for (int i = 0; i < 2; ++i) {
        auto* req = new Request;
        EXPECT_EQ(nullptr, Peer::Enqueue(ep, req));
        EXPECT_TRUE(Peer::Complete(ep, req, true));
        EXPECT_EQ(nullptr, Peer::Head(ep));
        delete req;
    }
}

TEST(RdmaWriteQueue, ConsumerWaitsForPublishedNodeToBeConnected) {
    EndpointTestOwner endpoint_owner;
    auto& ep = endpoint_owner.get();
    std::atomic<int> released{0};
    Request* a = TrackedRequest(released);
    Request* b = TrackedRequest(released);
    EXPECT_EQ(nullptr, Peer::Enqueue(ep, a));
    Request* prev = Peer::Publish(ep, b);  // producer is paused before linking
    EXPECT_EQ(a, prev);
    YieldBarrier barrier;
    auto entered = barrier.entered.get_future();
    Request* tail = nullptr;
    bool complete = true;
    std::thread consumer([&] {
        yield_barrier = &barrier;
        complete = Peer::Complete(ep, a, true, &tail);
        yield_barrier = nullptr;
    });
    EXPECT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(2)));
    b->next = prev;
    barrier.resume.set_value();
    consumer.join();
    EXPECT_FALSE(complete);
    EXPECT_EQ(b, tail);
    EXPECT_EQ(b, a->next);
    EXPECT_EQ(nullptr, b->next);
    Peer::Fail(ep, a, EPIPE);
    EXPECT_EQ(2, released.load());
}

TEST(RdmaWriteQueue, FailureDrainsBothUnconnectedAndLaterPublishedNodes) {
    EndpointTestOwner endpoint_owner;
    auto& ep = endpoint_owner.get();
    std::atomic<int> released{0};
    Request* a = TrackedRequest(released);
    Request* b = TrackedRequest(released);
    Request* c = TrackedRequest(released);
    Peer::Enqueue(ep, a);
    Request* prev = Peer::Publish(ep, b);
    YieldBarrier barrier;
    auto entered = barrier.entered.get_future();
    std::thread consumer([&] {
        yield_barrier = &barrier;
        Peer::Fail(ep, a, EPIPE);
        yield_barrier = nullptr;
    });
    EXPECT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(2)));
    // Another producer had passed the failure check before it was paused.
    // Its publication arrives after the consumer's first queue snapshot.
    EXPECT_EQ(b, Peer::Enqueue(ep, c));
    b->next = prev;
    barrier.resume.set_value();
    consumer.join();
    EXPECT_EQ(3, released.load());
    EXPECT_EQ(nullptr, Peer::Head(ep));
    IOBuf rejected;
    EXPECT_EQ(-1, ep.StartWrite(std::move(rejected)));
    EXPECT_EQ(EPIPE, errno);
}

TEST(RdmaWriteQueue, LatePublisherAfterFailureDrainsItsOwnEpoch) {
    EndpointTestOwner endpoint_owner;
    auto& ep = endpoint_owner.get();
    std::atomic<int> released{0};
    Request* a = TrackedRequest(released);
    Peer::Enqueue(ep, a);
    // Model a producer paused after its initial check, before exchange.
    EXPECT_EQ(0, Peer::Error(ep));
    Request* late = TrackedRequest(released);
    Peer::Fail(ep, a, ECONNRESET);
    EXPECT_EQ(nullptr, Peer::Enqueue(ep, late));
    EXPECT_EQ(ECONNRESET, Peer::Error(ep));
    Peer::Fail(ep, late, Peer::Error(ep));
    EXPECT_EQ(2, released.load());
    EXPECT_EQ(nullptr, Peer::Head(ep));
}

TEST(RdmaWriteQueue, PendingHandshakeFailureDrainsExactlyOnce) {
    EndpointTestOwner endpoint_owner;
    auto& ep = endpoint_owner.get();
    std::atomic<int> released{0};
    Request* a = TrackedRequest(released);
    Request* b = TrackedRequest(released);
    Peer::Enqueue(ep, a);
    Peer::Enqueue(ep, b);
    Peer::SetPending(ep, a);
    Peer::FailPending(ep, ECONNREFUSED);
    Peer::FailPending(ep, EIO);
    EXPECT_EQ(2, released.load());
    EXPECT_EQ(nullptr, Peer::Head(ep));
    EXPECT_EQ(nullptr, Peer::Pending(ep));
    EXPECT_EQ(ECONNREFUSED, Peer::Error(ep));
}

TEST(RdmaWriteQueue, SynchronousConnectFailureReleasesTheRealStartWriteRequest) {
    EndpointTestOwner endpoint_owner;
    auto& ep = endpoint_owner.get();
    ep.SetRemoteAddr("invalid-address", 1);
    std::atomic<int> released{0};
    IOBuf data = TrackedData(released);
    EXPECT_EQ(-1, ep.StartWrite(std::move(data)));
    EXPECT_EQ(EINVAL, errno);
    EXPECT_TRUE(data.empty());
    EXPECT_EQ(1, released.load());
    EXPECT_EQ(nullptr, Peer::Head(ep));
    EXPECT_EQ(nullptr, Peer::Pending(ep));
}

TEST(RdmaWriteQueue, ConcurrentSubmissionsDuringFailureReleaseEveryBufferOnce) {
    EndpointTestOwner endpoint_owner;
    auto& ep = endpoint_owner.get();
    std::atomic<int> released{0};
    Request* owner = TrackedRequest(released);
    Peer::Enqueue(ep, owner);
    constexpr int threads = 8;
    constexpr int requests = 64;
    std::promise<void> start;
    auto go = start.get_future().share();
    std::vector<std::thread> producers;
    std::atomic<int> unexpected_error{0};
    for (int i = 0; i < threads; ++i) {
        producers.emplace_back([&] {
            go.wait();
            for (int j = 0; j < requests; ++j) {
                IOBuf data = TrackedData(released);
                if (ep.StartWrite(std::move(data)) < 0 && errno != EPIPE) {
                    ++unexpected_error;
                }
            }
        });
    }
    start.set_value();
    Peer::Fail(ep, owner, EPIPE);
    for (auto& producer : producers) producer.join();
    EXPECT_EQ(0, unexpected_error.load());
    EXPECT_EQ(1 + threads * requests, released.load());
    EXPECT_EQ(nullptr, Peer::Head(ep));
}

}  // namespace
}  // namespace fast

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
