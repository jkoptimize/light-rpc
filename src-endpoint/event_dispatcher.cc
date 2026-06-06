#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstdint>
#include "event_dispatcher.h"
#include "fast_log.h"

namespace fast {

struct EventContext {
    EventDispatcher::Callback cb;
    void*                     user_data;
};

EventDispatcher::EventDispatcher() {
    _epfd = epoll_create(1);
    CHECK(_epfd >= 0);

    _efd = eventfd(0, EFD_NONBLOCK);
    CHECK(_efd >= 0);

    // Register eventfd so Stop() can wake epoll_wait
    epoll_event evt = {};
    evt.events   = EPOLLIN;
    evt.data.ptr = nullptr;
    CHECK(epoll_ctl(_epfd, EPOLL_CTL_ADD, _efd, &evt) == 0);
}

EventDispatcher::~EventDispatcher() {
    if (_efd >= 0)  { close(_efd);  _efd  = -1; }
    if (_epfd >= 0) { close(_epfd); _epfd = -1; }
}

int EventDispatcher::AddFd(int fd, Callback cb, void* user_data) {
    auto* ctx = new EventContext{cb, user_data};

    epoll_event evt = {};
    evt.events   = EPOLLIN | EPOLLET;
    evt.data.ptr = ctx;
    int ret = epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &evt);
    if (ret < 0) {
        delete ctx;
        return -1;
    }
    return 0;
}

int EventDispatcher::RemoveFd(int fd) {
    epoll_event evt = {};
    int ret = epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, &evt);
    // Note: the EventContext is NOT freed here — caller must manage its lifetime.
    // For comp_channel fds, the EventContext lives as long as the fd is registered.
    return ret;
}

void EventDispatcher::Run() {
    epoll_event events[32];
    while (!_stop.load(std::memory_order_relaxed)) {
        int n = epoll_wait(_epfd, events, 32, -1);
        if (_stop.load(std::memory_order_relaxed)) break;

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.ptr == nullptr) {
                // eventfd wake — consume the event
                uint64_t val;
                read(_efd, &val, sizeof(val));
                continue;
            }
            auto* ctx = static_cast<EventContext*>(events[i].data.ptr);
            ctx->cb(ctx->user_data, events[i].events);
        }
    }
}

void EventDispatcher::Stop() {
    _stop.store(true, std::memory_order_relaxed);
    // Write to eventfd to wake epoll_wait
    uint64_t val = 1;
    write(_efd, &val, sizeof(val));
    if (_thread.joinable()) _thread.join();
}

}  // namespace fast
