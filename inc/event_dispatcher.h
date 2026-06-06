#pragma once

#include <atomic>
#include <thread>
#include <cstdint>

namespace fast {

// Lightweight epoll-based event dispatcher.
// Thread is started in constructor, stopped in destructor.
// Usage from outside: just AddFd / RemoveFd.

class EventDispatcher {
public:
    using Callback = void (*)(void* user_data, uint32_t events);

    EventDispatcher();
    ~EventDispatcher();

    // Register fd for EPOLLIN | EPOLLET. cb is called on events.
    int AddFd(int fd, Callback cb, void* user_data);

    // Remove fd from epoll.
    int RemoveFd(int fd);

private:
    void RunEpollLoop();

    int                 _epfd = -1;
    int                 _efd  = -1;  // eventfd for wake
    std::thread         _thread;
    std::atomic<bool>   _stop{false};
};

}  // namespace fast
