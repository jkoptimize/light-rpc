#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include "event_dispatcher.h"
#include "fast_log.h"

namespace fast {
EventDispatcher& EventDispatcher::GetInstance() {
    static EventDispatcher instance;
    return instance;
}
EventDispatcher::EventDispatcher() {
    _epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(_epfd >= 0);
    _efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(_efd >= 0);
    epoll_event evt = {};
    evt.events = EPOLLIN;
    evt.data.u64 = INVALID_REGISTRATION;
    CHECK(epoll_ctl(_epfd, EPOLL_CTL_ADD, _efd, &evt) == 0);
    _thread = std::thread(&EventDispatcher::RunEpollLoop, this);
}
EventDispatcher::~EventDispatcher() {
    _stop.store(true, std::memory_order_relaxed);
    uint64_t val = 1;
    while (write(_efd, &val, sizeof(val)) < 0 && errno == EINTR) {}
    if (_thread.joinable()) _thread.join();
    for (const auto& entry : _fd_map) IOEventData::SetFailedById(entry.second);
    close(_efd);
    close(_epfd);
}
int EventDispatcher::RegisterEvent(int fd, InputCallback in_cb, OutputCallback out_cb,
                                  void* user_data, uint32_t events,
                                  Registration* registration,
                                  std::function<void()> on_recycled) {
    IOEventDataOptions options{in_cb, out_cb, user_data, std::move(on_recycled)};
    Registration id;
    if (IOEventData::Create(&id, options) != 0) return -1;
    int error = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_fd_map.count(fd)) {
            error = EEXIST;
        } else {
            epoll_event evt = {};
            evt.data.u64 = id;
            evt.events = events | EPOLLERR | EPOLLHUP;
            // Install both owner identity and output before publishing to epoll.
            _fd_map.emplace(fd, id);
            *registration = id;
            if (epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &evt) < 0) {
                error = errno;
                _fd_map.erase(fd);
                *registration = INVALID_REGISTRATION;
            }
        }
    }
    if (error) {
        IOEventData::SetFailedById(id);
        errno = error;
        return -1;
    }
    return 0;
}
int EventDispatcher::UnregisterEvent(int fd, Registration registration) {
    if (registration == INVALID_REGISTRATION) return 0;
    int rc = 0;
    int error = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _fd_map.find(fd);
        if (it == _fd_map.end() || it->second != registration) return 0;
        rc = epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr);
        error = errno;
        _fd_map.erase(it);
    }
    // Callback completion may notify the owner; never run it under _mutex.
    IOEventData::SetFailedById(registration);
    errno = error;
    return rc;
}
void EventDispatcher::Dispatch(Registration id, uint32_t events, bool output) {
    VersionedRefWithIdUniquePtr<IOEventData> data;
    if (IOEventData::Address(id, &data) != 0) return;
    data->Call(output, events);
}
void EventDispatcher::RunEpollLoop() {
    epoll_event events[32];
    while (!_stop.load(std::memory_order_relaxed)) {
        const int n = epoll_wait(_epfd, events, 32, -1);
        if (_stop.load(std::memory_order_relaxed)) break;
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i) {
            const auto id = events[i].data.u64;
            const uint32_t ev = events[i].events;
            if (id == INVALID_REGISTRATION) {
                uint64_t val;
                read(_efd, &val, sizeof(val));
                continue;
            }
            if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP)) Dispatch(id, ev, false);
            if (ev & (EPOLLOUT | EPOLLERR | EPOLLHUP)) Dispatch(id, ev, true);
        }
    }
}
}  // namespace fast
