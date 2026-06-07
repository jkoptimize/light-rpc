#pragma once

#include <atomic>
#include <thread>
#include <cstdint>

namespace fast {

// Epoll-based event dispatcher, one global instance.
// Thread auto-started in constructor.
//
// AddConsumer:  register fd for EPOLLIN  (listen / comp_channel / data)
// RegisterEvent: register fd for EPOLLOUT (connect monitoring, pollin=false)
//                or add EPOLLIN to existing EPOLLOUT fd (pollin=true)

class EventDispatcher {
public:
    using InputCallback  = void (*)(void* user_data, uint32_t events);
    using OutputCallback = void (*)(void* user_data, uint32_t events);

    static EventDispatcher& GetInstance();

    EventDispatcher();
    ~EventDispatcher();

    // EPOLL_CTL_ADD with EPOLLIN | EPOLLET.
    // Callback fires on EPOLLIN | EPOLLERR | EPOLLHUP.
    int AddConsumer(int fd, InputCallback cb, void* user_data);

    // pollin == false: EPOLL_CTL_ADD with EPOLLOUT | EPOLLET (connect).
    // pollin == true:  EPOLL_CTL_MOD to add EPOLLIN (fd already watched for EPOLLOUT).
    // Callback fires on EPOLLOUT | EPOLLERR | EPOLLHUP (or EPOLLIN when pollin=true).
    int RegisterEvent(int fd, OutputCallback cb, void* user_data, bool pollin);

    // EPOLL_CTL_DEL — remove fd entirely from epoll.
    int RemoveConsumer(int fd);

private:
    void RunEpollLoop();

    int                 _epfd = -1;
    int                 _efd  = -1;  // eventfd for wake
    std::thread         _thread;
    std::atomic<bool>   _stop{false};
};

}  // namespace fast
