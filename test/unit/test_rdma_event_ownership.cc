#include "test/endpoint_test_owner.h"
#include <gtest/gtest.h>
#include <future>
#include <thread>

#include "fast_rdma_endpoint.h"

namespace fast {

// For unit tests only: no channel, CQ or QP is created or polled.
class FastRdmaEndpointEventTestPeer {
public:
    static bool AddEvent(FastRdmaEndpoint& ep) { return ep.AddReadEvent(); }
    static bool MoreEvents(FastRdmaEndpoint& ep, int* progress) {
        return ep.MoreReadEvents(progress);
    }
    static int InitialProgress() { return FastRdmaEndpoint::PROGRESS_INIT; }
    static void StopPolling(FastRdmaEndpoint& ep) { ep.StopCqPolling(); }
};

namespace {
using Peer = FastRdmaEndpointEventTestPeer;

TEST(RdmaEventOwnership, DrainedConsumerReleasesOwnership) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    ASSERT_TRUE(Peer::AddEvent(ep));
    int progress = Peer::InitialProgress();
    EXPECT_FALSE(Peer::MoreEvents(ep, &progress));
    // A subsequent event must be allowed to start a new consumer.
    EXPECT_TRUE(Peer::AddEvent(ep));
    progress = Peer::InitialProgress();
    EXPECT_FALSE(Peer::MoreEvents(ep, &progress));
}

TEST(RdmaEventOwnership, ArrivingEventsStayWithCurrentConsumer) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    ASSERT_TRUE(Peer::AddEvent(ep));
    EXPECT_FALSE(Peer::AddEvent(ep));
    EXPECT_FALSE(Peer::AddEvent(ep));
    int progress = Peer::InitialProgress();
    EXPECT_TRUE(Peer::MoreEvents(ep, &progress));
    EXPECT_EQ(3, progress);
    // Another event may arrive while the same consumer drains the next batch.
    EXPECT_FALSE(Peer::AddEvent(ep));
    EXPECT_TRUE(Peer::MoreEvents(ep, &progress));
    EXPECT_EQ(4, progress);
    EXPECT_FALSE(Peer::MoreEvents(ep, &progress));
}

TEST(RdmaEventOwnership, EventBeforeReleaseKeepsExistingConsumer) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    ASSERT_TRUE(Peer::AddEvent(ep));
    std::promise<void> event_published;
    auto published = event_published.get_future();
    bool started_another_consumer = true;
    std::thread dispatcher([&] {
        started_another_consumer = Peer::AddEvent(ep);
        event_published.set_value();
    });
    published.wait();
    int progress = Peer::InitialProgress();
    const bool keep_processing = Peer::MoreEvents(ep, &progress);
    dispatcher.join();
    EXPECT_FALSE(started_another_consumer);
    EXPECT_TRUE(keep_processing);
    EXPECT_EQ(2, progress);
    EXPECT_FALSE(Peer::MoreEvents(ep, &progress));
}

TEST(RdmaEventOwnership, EventAfterReleaseStartsNextConsumer) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    ASSERT_TRUE(Peer::AddEvent(ep));
    std::promise<void> ownership_released;
    auto released = ownership_released.get_future();
    bool started_next_consumer = false;
    std::thread dispatcher([&] {
        released.wait();
        started_next_consumer = Peer::AddEvent(ep);
    });
    int progress = Peer::InitialProgress();
    const bool keep_processing = Peer::MoreEvents(ep, &progress);
    ownership_released.set_value();
    dispatcher.join();
    EXPECT_FALSE(keep_processing);
    EXPECT_TRUE(started_next_consumer);
    progress = Peer::InitialProgress();
    EXPECT_FALSE(Peer::MoreEvents(ep, &progress));
}

TEST(RdmaEventOwnership, PollingFailurePreventsRestart) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    ASSERT_TRUE(Peer::AddEvent(ep));
    EXPECT_FALSE(Peer::AddEvent(ep));
    Peer::StopPolling(ep);
    EXPECT_FALSE(Peer::AddEvent(ep));
    // Stopping is idempotent and cannot make the failed consumer restartable.
    Peer::StopPolling(ep);
    EXPECT_FALSE(Peer::AddEvent(ep));
}

TEST(RdmaEventOwnership, StoppedEndpointDoesNotAcquireOwnership) {
    EndpointTestOwner owner;
    auto& ep = owner.get();
    Peer::StopPolling(ep);
    EXPECT_FALSE(Peer::AddEvent(ep));
    // The real callback must also return without creating a thread or using RDMA.
    FastRdmaEndpoint::OnCompChannelEvent(reinterpret_cast<void*>(ep.id()), 0);
}

}  // namespace
}  // namespace fast
