#pragma once

#include <atomic>
#include <thread>
#include <cstdint>

namespace fast {

// Epoll-based event dispatcher, one global instance.
// Thread auto-started in constructor.
//
// AddConsumer: register fd for EPOLLIN  (listen / comp_channel)
// RegisterEvent: register fd for EPOLLOUT (connect completion)

class EventDispatcher {
public:
    using InputCallback  = void (*)(void* user_data, uint32_t events);
    using OutputCallback = void (*)(void* user_data, uint32_t events);

    static EventDispatcher& GetInstance();

    EventDispatcher();
    ~EventDispatcher();

    // Register fd for EPOLLIN | EPOLLET.
    // Callback fires on EPOLLIN | EPOLLERR | EPOLLHUP.
    int AddConsumer(int fd, InputCallback cb, void* user_data);

    // Register fd for EPOLLOUT | EPOLLET (or modify existing).
    // Callback fires on EPOLLOUT | EPOLLERR | EPOLLHUP.
    int RegisterEvent(int fd, OutputCallback cb, void* user_data);

    // Remove fd from epoll (and free context).
    int RemoveConsumer(int fd);

private:
    void RunEpollLoop();

    int                 _epfd = -1;
    int                 _efd  = -1;  // eventfd for wake
    std::thread         _thread;
    std::atomic<bool>   _stop{false};
};

}  // namespace fast
