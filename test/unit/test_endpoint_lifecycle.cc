#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include "fast_channel.h"
#include "fast_server.h"
#include "fast_utils.h"
#include "test/endpoint_test_owner.h"

namespace fast {
// For unit tests only: all endpoints have null QPs/CQs; no verbs are invoked.
class FastRdmaEndpointLifecycleTestPeer {
public:
    static int Read(FastRdmaEndpoint& ep, int fd) {
        char c;
        return ep.ReadFromFd(fd, &c, 1);
    }
    static void Pending(FastRdmaEndpoint& ep, IOBuf&& data) {
        auto* req = new FastRdmaEndpoint::WriteRequest;
        req->data.swap(data);
        req->next = ep.PublishWriteRequest(req);
        ep._pending_keepwrite_req.store(req);
    }
};
class FastChannelLifecycleTestPeer {
public:
    using Request = FastChannel::PendingRequest;
    static std::unique_ptr<FastChannel> Create(FastRdmaEndpoint* ep) {
        return std::unique_ptr<FastChannel>(new FastChannel(ep));
    }
    static bool Register(FastChannel& ch, uint32_t id, Request* req) {
        return ch.RegisterPending(id, req);
    }
    static void Remove(FastChannel& ch, uint32_t id) {
        std::lock_guard<std::mutex> lock(ch.pending_mutex_);
        ch.pending_map_.erase(id);
    }
    static void Response(FastChannel& ch, IOBuf& frame) { ch.OnProcessResponse(frame, &ch); }
};
namespace {
using namespace std::chrono_literals;
using Peer = FastRdmaEndpointLifecycleTestPeer;
using ChannelPeer = FastChannelLifecycleTestPeer;
IOBuf TrackedByte(std::atomic<int>& releases) {
    IOBuf data;
    data.append_user_data_with_meta(new char[1]{0}, 1, [&releases](void* ptr) {
        ++releases;
        delete[] static_cast<char*>(ptr);
    }, 0);
    return data;
}
IOBuf ResponseFrame(uint32_t rpc, uint32_t error) {
    auto* data = new uint32_t[5]{htonl(20), htonl(MSG_NORMAL_RESPONSE),
                                htonl(rpc), htonl(error), 0};
    IOBuf frame;
    frame.append_user_data_with_meta(data, 20,
        [](void* p) { delete[] static_cast<uint32_t*>(p); }, 0);
    return frame;
}
TEST(EndpointLifecycle, FailureRetainsBuffersUntilLastReference) {
    std::atomic<int> releases{0}, recycled{0}, failures{0};
    EndpointTestOwner owner;
    auto reference = owner.release();
    auto* ep = reference.get();
    const auto id = ep->id();
    ep->SetFailureHandler([&](int) { ++failures; });
    ep->SetRecycleHandler([&] { ++recycled; });
    Peer::Pending(*ep, TrackedByte(releases));
    EXPECT_EQ(0, ep->SetFailed(EPIPE));
    EXPECT_EQ(-1, ep->SetFailed(ECONNRESET));
    EXPECT_EQ(EPIPE, ep->error());
    EXPECT_EQ(1, failures.load());
    EXPECT_EQ(0, releases.load());
    EndpointUniquePtr addressed;
    EXPECT_EQ(-1, FastRdmaEndpoint::Address(id, &addressed));
    EXPECT_EQ(1, FastRdmaEndpoint::AddressFailedAsWell(id, &addressed));
    reference.reset();
    EXPECT_EQ(0, recycled.load());
    addressed.reset();
    EXPECT_EQ(1, recycled.load());
    EXPECT_EQ(1, releases.load());
    EXPECT_EQ(-1, FastRdmaEndpoint::AddressFailedAsWell(id, &addressed));
}
TEST(EndpointLifecycle, ReusedSlotRejectsOldIdAndResetsState) {
    EndpointTestOwner owner;
    auto first = owner.release();
    const auto old_id = first->id();
    first->SetRemoteAddr("old", 123);
    first->SetFailed(EPIPE);
    first.reset();
    EndpointTestOwner next;
    auto& ep = next.get();
    EXPECT_EQ(SlotOfVRefId<FastRdmaEndpoint>(old_id).value,
              SlotOfVRefId<FastRdmaEndpoint>(ep.id()).value);
    EXPECT_NE(old_id, ep.id());
    EXPECT_FALSE(ep.Failed());
    EXPECT_EQ(0, ep.error());
    EXPECT_FALSE(ep.IsHandshakeOk());
    EndpointUniquePtr stale;
    EXPECT_EQ(-1, FastRdmaEndpoint::Address(old_id, &stale));
    FastRdmaEndpoint::OnCompChannelEvent(reinterpret_cast<void*>(old_id), 0);
}
TEST(EndpointLifecycle, FailedWorkerRecyclesWithoutWaitingForItself) {
    std::promise<void> recycled;
    EndpointTestOwner owner;
    auto reference = owner.release();
    reference->SetRecycleHandler([&] { recycled.set_value(); });
    auto done = recycled.get_future();
    auto worker = std::async(std::launch::async, [reference = std::move(reference)]() mutable {
        reference->SetFailed(EPROTO);
        reference.reset();
    });
    EXPECT_EQ(std::future_status::ready, done.wait_for(2s));
    worker.get();
}
TEST(EndpointLifecycle, FailureCancelsTcpReadWithoutClosingUnderReader) {
    int fd[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fd));
    auto cleanup = MakeScopeGuard([&] { close(fd[0]); close(fd[1]); });
    EndpointTestOwner owner;
    auto& ep = owner.get();
    EndpointUniquePtr worker_ref;
    ep.ReAddress(&worker_ref);
    std::promise<void> started;
    auto reader = std::async(std::launch::async, [&, ref = std::move(worker_ref)] {
        started.set_value();
        const int rc = Peer::Read(*ref, fd[0]);
        return std::make_pair(rc, errno);
    });
    started.get_future().wait();
    ep.SetFailed(ECANCELED);
    EXPECT_EQ(std::future_status::ready, reader.wait_for(2s));
    EXPECT_EQ(std::make_pair(-1, ECANCELED), reader.get());
    EXPECT_EQ(1, write(fd[1], "x", 1));
}
TEST(EndpointLifecycle, DetachedMessageRetainsEndpointUntilHandlerReturns) {
    std::promise<void> entered, resume, recycled;
    auto released = resume.get_future().share();
    auto done = recycled.get_future();
    EndpointTestOwner owner;
    auto ref = owner.release();
    ref->SetRecycleHandler([&] { recycled.set_value(); });
    ref->msg_dispatcher().SetHandler([&](IOBuf&, void*) {
        entered.set_value();
        released.wait();
        return 0;
    }, nullptr);
    auto frame = ResponseFrame(1, ERR_INTERNAL);
    ASSERT_EQ(1, ref->msg_dispatcher().ProcessNewMessage(frame, ref.get()));
    entered.get_future().wait();
    ref->SetFailed();
    ref.reset();
    EXPECT_EQ(std::future_status::timeout, done.wait_for(0s));
    resume.set_value();
    EXPECT_EQ(std::future_status::ready, done.wait_for(2s));
}
TEST(ChannelLifecycle, FailureNotifiesRequestsAndRejectsLateRegistration) {
    EndpointTestOwner owner;
    auto ref = owner.release();
    auto channel = ChannelPeer::Create(ref.get());
    ChannelPeer::Request first, late;
    ASSERT_TRUE(ChannelPeer::Register(*channel, 1, &first));
    ref->SetFailed(EPIPE);
    EXPECT_TRUE(first.done);
    EXPECT_EQ(EPIPE, first.transport_error);
    EXPECT_FALSE(ChannelPeer::Register(*channel, 2, &late));
    EXPECT_TRUE(late.done);
    EXPECT_EQ(EPIPE, late.transport_error);
    ChannelPeer::Remove(*channel, 1);
    ref.reset();
    channel->Close();
    channel->Close();
}
TEST(ChannelLifecycle, ConcurrentRegistrationCannotMissFailure) {
    EndpointTestOwner owner;
    auto ref = owner.release();
    auto channel = ChannelPeer::Create(ref.get());
    constexpr unsigned n = 64;
    ChannelPeer::Request requests[n];
    std::promise<void> start;
    auto go = start.get_future().share();
    std::thread registerer([&] {
        go.wait();
        for (unsigned i = 0; i < n; ++i) ChannelPeer::Register(*channel, i, &requests[i]);
    });
    start.set_value();
    ref->SetFailed(ECONNRESET);
    registerer.join();
    for (unsigned i = 0; i < n; ++i) {
        EXPECT_TRUE(requests[i].done);
        EXPECT_EQ(ECONNRESET, requests[i].transport_error);
        ChannelPeer::Remove(*channel, i);
    }
    ref.reset();
}
TEST(ChannelLifecycle, ResponseAndFailureKeepFirstCompletion) {
    EndpointTestOwner owner;
    auto ref = owner.release();
    auto channel = ChannelPeer::Create(ref.get());
    ChannelPeer::Request first, second;
    ASSERT_TRUE(ChannelPeer::Register(*channel, 1, &first));
    ASSERT_TRUE(ChannelPeer::Register(*channel, 2, &second));
    auto response = ResponseFrame(1, ERR_INTERNAL);
    ChannelPeer::Response(*channel, response);
    ref->SetFailed(EPIPE);
    auto late = ResponseFrame(2, ERR_BAD_RESPONSE);
    ChannelPeer::Response(*channel, late);
    EXPECT_EQ(0, first.transport_error);
    EXPECT_EQ(ERR_INTERNAL, first.error_code);
    EXPECT_EQ(EPIPE, second.transport_error);
    EXPECT_EQ(0u, second.error_code);
    ChannelPeer::Remove(*channel, 1);
    ChannelPeer::Remove(*channel, 2);
    ref.reset();
}
TEST(ChannelLifecycle, CloseWakesCallerWithoutHoldingPendingMutex) {
    EndpointTestOwner owner;
    auto ref = owner.release();
    auto channel = ChannelPeer::Create(ref.get());
    ChannelPeer::Request request;
    ASSERT_TRUE(ChannelPeer::Register(*channel, 1, &request));
    auto caller = std::async(std::launch::async, [&, call = std::move(ref)]() mutable {
        {
            std::unique_lock<std::mutex> lock(request.mutex);
            request.cv.wait(lock, [&] { return request.done; });
        }
        ChannelPeer::Remove(*channel, 1);
        call.reset();
    });
    auto closed = std::async(std::launch::async, [&] { channel->Close(); });
    EXPECT_EQ(std::future_status::ready, caller.wait_for(2s));
    EXPECT_EQ(std::future_status::ready, closed.wait_for(2s));
    EXPECT_EQ(ECANCELED, request.transport_error);
}
TEST(ServerLifecycle, CloseRetainsAsyncServiceReference) {
    FastServer server("127.0.0.1", 0);
    EndpointTestOwner owner;
    auto ref = owner.release();
    std::promise<void> failed;
    auto stopped = failed.get_future();
    ref->SetFailureHandler([&](int) { server.NotifyEndpointFailed(); failed.set_value(); });
    server.AddEndpoint(ref.get());
    auto closed = std::async(std::launch::async, [&] { server.Close(); });
    stopped.wait();
    EXPECT_EQ(std::future_status::timeout, closed.wait_for(0s));
    ref.reset();
    EXPECT_EQ(std::future_status::ready, closed.wait_for(2s));
}
} // namespace
} // namespace fast
