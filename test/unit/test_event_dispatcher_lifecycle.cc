#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include "event_dispatcher.h"
#include "fast_utils.h"

namespace fast {
// For unit tests only: replay an id already fetched by epoll deterministically.
class EventDispatcherLifecycleTestPeer {
public:
    static void Dispatch(EventDispatcher& d, VRefId id) { d.Dispatch(id, EPOLLIN, false); }
};
namespace {
using namespace std::chrono_literals;
struct Callback {
    EventDispatcher* dispatcher;
    int fd;
    EventDispatcher::Registration id = EventDispatcher::INVALID_REGISTRATION;
    bool self_remove = false;
    std::promise<void> entered, resume, recycled;
    std::shared_future<void> released = resume.get_future().share();
    std::atomic<int> calls{0};
    static void Run(void* arg, uint32_t) {
        auto* self = static_cast<Callback*>(arg);
        ++self->calls;
        if (self->self_remove) self->dispatcher->UnregisterEvent(self->fd, self->id);
        self->entered.set_value();
        self->released.wait();
    }
};
void Signal(int fd) { const uint64_t one = 1; ASSERT_EQ(8, write(fd, &one, 8)); }

TEST(EventDispatcherLifecycle, UnregisterInvalidatesWithoutJoiningRunningCallback) {
    EventDispatcher dispatcher;
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    auto cleanup = MakeScopeGuard([&] { close(fd); });
    Callback callback{&dispatcher, fd};
    auto recycled = callback.recycled.get_future();
    ASSERT_EQ(0, dispatcher.RegisterEvent(fd, Callback::Run, nullptr, &callback,
        EPOLLIN | EPOLLET, &callback.id, [&] { callback.recycled.set_value(); }));
    Signal(fd);
    callback.entered.get_future().wait();
    EXPECT_EQ(0, dispatcher.UnregisterEvent(fd, callback.id));
    VersionedRefWithIdUniquePtr<IOEventData> ref;
    EXPECT_EQ(-1, IOEventData::Address(callback.id, &ref));
    EXPECT_EQ(std::future_status::timeout, recycled.wait_for(0s));
    callback.resume.set_value();
    ASSERT_EQ(std::future_status::ready, recycled.wait_for(2s));
    EXPECT_EQ(1, callback.calls.load());
}

TEST(EventDispatcherLifecycle, SelfUnregisterRecyclesAfterCallbackReturns) {
    EventDispatcher dispatcher;
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    auto cleanup = MakeScopeGuard([&] { close(fd); });
    Callback callback{&dispatcher, fd};
    callback.self_remove = true;
    auto recycled = callback.recycled.get_future();
    ASSERT_EQ(0, dispatcher.RegisterEvent(fd, Callback::Run, nullptr, &callback,
        EPOLLIN | EPOLLET, &callback.id, [&] { callback.recycled.set_value(); }));
    Signal(fd);
    callback.entered.get_future().wait();
    EXPECT_EQ(0, dispatcher.UnregisterEvent(fd, callback.id));
    EXPECT_EQ(std::future_status::timeout, recycled.wait_for(0s));
    callback.resume.set_value();
    EXPECT_EQ(std::future_status::ready, recycled.wait_for(2s));
}

TEST(EventDispatcherLifecycle, OldEventAndRegistrationCannotAffectReusedFdAndSlot) {
    EventDispatcher dispatcher;
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    auto cleanup = MakeScopeGuard([&] { close(fd); });
    EventDispatcher::Registration old_id, new_id;
    std::atomic<int> calls{0};
    auto cb = [](void* arg, uint32_t) { ++*static_cast<std::atomic<int>*>(arg); };
    ASSERT_EQ(0, dispatcher.RegisterEvent(fd, cb, nullptr, &calls, EPOLLIN, &old_id));
    ASSERT_EQ(0, dispatcher.UnregisterEvent(fd, old_id));
    close(fd);
    const int replacement = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(replacement, 0);
    if (replacement != fd) { ASSERT_EQ(fd, dup2(replacement, fd)); close(replacement); }
    ASSERT_EQ(0, dispatcher.RegisterEvent(fd, cb, nullptr, &calls, EPOLLIN, &new_id));
    EXPECT_EQ(SlotOfVRefId<IOEventData>(old_id).value, SlotOfVRefId<IOEventData>(new_id).value);
    EXPECT_NE(old_id, new_id);
    EXPECT_EQ(0, dispatcher.UnregisterEvent(fd, old_id));
    EventDispatcherLifecycleTestPeer::Dispatch(dispatcher, old_id);
    EXPECT_EQ(0, calls.load());
    EventDispatcherLifecycleTestPeer::Dispatch(dispatcher, new_id);
    EXPECT_EQ(1, calls.load());
    EXPECT_EQ(0, dispatcher.UnregisterEvent(fd, new_id));
}

TEST(EventDispatcherLifecycle, FailedRegistrationReleasesContextOnce) {
    EventDispatcher dispatcher;
    int recycled = 0;
    EventDispatcher::Registration id = EventDispatcher::INVALID_REGISTRATION;
    EXPECT_EQ(-1, dispatcher.RegisterEvent(-1, nullptr, nullptr, nullptr, EPOLLIN,
        &id, [&] { ++recycled; }));
    EXPECT_EQ(1, recycled);
    EXPECT_EQ(EventDispatcher::INVALID_REGISTRATION, id);
}
} // namespace
} // namespace fast
