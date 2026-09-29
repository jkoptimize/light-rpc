#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "versioned_ref_with_id.h"

namespace fast {

using EventCallback = void (*)(void*, uint32_t);

// Ported from brpc IOEventData: epoll stores a versioned id, and dispatch
// acquires a reference before reading callbacks. user_data has its own lifetime
// (endpoint callbacks carry a VRefId, never an unprotected endpoint pointer).
struct IOEventDataOptions {
    EventCallback input_cb = nullptr;
    EventCallback output_cb = nullptr;
    void* user_data = nullptr;
    // Optional owner completion for non-pooled owners such as FastServer.
    std::function<void()> on_recycled;
};

class IOEventData : public VersionedRefWithId<IOEventData> {
public:
    explicit IOEventData(Forbidden f) : VersionedRefWithId<IOEventData>(f) {}
    void Call(bool output, uint32_t events) {
        auto cb = output ? options_.output_cb : options_.input_cb;
        if (cb) cb(options_.user_data, events);
    }
private:
    friend class VersionedRefWithId<IOEventData>;
    int OnCreated(const IOEventDataOptions& options) { options_ = options; return 0; }
    void BeforeRecycled() {
        auto done = std::move(options_.on_recycled);
        options_ = {};
        if (done) done();
    }
    IOEventDataOptions options_;
};

class EventDispatcher {
public:
    using InputCallback = EventCallback;
    using OutputCallback = EventCallback;
    using Registration = VRefId;
    static constexpr Registration INVALID_REGISTRATION = INVALID_VREF_ID;
    static EventDispatcher& GetInstance();
    EventDispatcher();
    ~EventDispatcher();

    int RegisterEvent(int fd, InputCallback in_cb, OutputCallback out_cb,
                      void* user_data, uint32_t events, Registration* registration,
                      std::function<void()> on_recycled = {});
    // Invalidates the registration; already addressed callbacks finish under
    // their IOEventData reference. Does not wait, including on the event thread.
    // Owners of raw user_data must wait for on_recycled before destroying it.
    int UnregisterEvent(int fd, Registration registration);

private:
    // For unit tests only: replay a previously fetched event id.
    friend class EventDispatcherLifecycleTestPeer;
    void RunEpollLoop();
    void Dispatch(Registration id, uint32_t events, bool output);
    int _epfd = -1;
    int _efd = -1;
    std::thread _thread;
    std::atomic<bool> _stop{false};
    std::mutex _mutex;
    // Only registration changes take this lock; event dispatch uses Address.
    std::unordered_map<int, Registration> _fd_map;
};
}  // namespace fast
