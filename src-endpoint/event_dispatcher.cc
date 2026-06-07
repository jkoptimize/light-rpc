#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include "event_dispatcher.h"
#include "fast_log.h"

namespace fast {

EventDispatcher& EventDispatcher::GetInstance() {
    static EventDispatcher instance;
    return instance;
}

EventDispatcher::EventDispatcher() {
    _epfd = epoll_create(1);
    CHECK(_epfd >= 0);

    _efd = eventfd(0, EFD_NONBLOCK);
    CHECK(_efd >= 0);

    epoll_event evt = {};
    evt.events   = EPOLLIN;
    evt.data.ptr = nullptr;
    CHECK(epoll_ctl(_epfd, EPOLL_CTL_ADD, _efd, &evt) == 0);

    _thread = std::thread(&EventDispatcher::RunEpollLoop, this);
}

EventDispatcher::~EventDispatcher() {
    _stop.store(true, std::memory_order_relaxed);
    uint64_t val = 1;
    write(_efd, &val, sizeof(val));
    if (_thread.joinable()) _thread.join();

    for (auto& kv : _fd_map) delete kv.second;
    _fd_map.clear();

    if (_efd >= 0)  { close(_efd);  _efd  = -1; }
    if (_epfd >= 0) { close(_epfd); _epfd = -1; }
}

int EventDispatcher::RegisterEvent(int fd, InputCallback in_cb, OutputCallback out_cb,
                                    void* user_data, uint32_t events) {
    auto* ctx = new EventContext{in_cb, out_cb, user_data};

    epoll_event evt = {};
    evt.data.ptr = ctx;
    evt.events = events | EPOLLERR | EPOLLHUP;
    if (epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &evt) < 0) {
        delete ctx;
        return -1;
    }
    _fd_map[fd] = ctx;
    return 0;
}

int EventDispatcher::UnregisterEvent(int fd) {
    epoll_event evt = {};
    int ret = epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, &evt);

    auto it = _fd_map.find(fd);
    if (it != _fd_map.end()) {
        delete it->second;
        _fd_map.erase(it);
    }
    return ret;
}

void EventDispatcher::RunEpollLoop() {
    epoll_event events[32];
    while (!_stop.load(std::memory_order_relaxed)) {
        int n = epoll_wait(_epfd, events, 32, -1);
        if (_stop.load(std::memory_order_relaxed)) break;

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            uint32_t ev = events[i].events;

            if (events[i].data.ptr == nullptr) {
                uint64_t val;
                read(_efd, &val, sizeof(val));
                continue;
            }

            auto* ctx = static_cast<EventContext*>(events[i].data.ptr);

            if (ctx->input_cb && (ev & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
                ctx->input_cb(ctx->user_data, ev);
            }
            if (ctx->output_cb && (ev & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
                ctx->output_cb(ctx->user_data, ev);
            }
        }
    }
}

}  // namespace fast
