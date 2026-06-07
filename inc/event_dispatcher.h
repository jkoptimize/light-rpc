#pragma once

#include <atomic>
#include <thread>
#include <cstdint>

namespace fast {

// Epoll-based event dispatcher, one global instance.
// Thread auto-started in constructor.
//
// AddConsumer:     EPOLL_CTL_ADD  with EPOLLIN  | EPOLLET
// RegisterEvent:   EPOLL_CTL_ADD  with EPOLLOUT | EPOLLET (pollin=false)
//                  EPOLL_CTL_MOD  to add EPOLLIN        (pollin=true)
// UnregisterEvent: EPOLL_CTL_MOD  to keep EPOLLIN only  (pollin=true)
//                  EPOLL_CTL_DEL  remove entirely       (pollin=false)
// RemoveConsumer:  EPOLL_CTL_DEL  remove entirely

class EventDispatcher {
public:
    using InputCallback  = void (*)(void* user_data, uint32_t events);
    using OutputCallback = void (*)(void* user_data, uint32_t events);

    static EventDispatcher& GetInstance();

    EventDispatcher();
    ~EventDispatcher();

    int AddConsumer(int fd, InputCallback cb, void* user_data);

    int RegisterEvent(int fd, InputCallback in_cb, OutputCallback out_cb,
                       void* user_data, bool pollin);

    int UnregisterEvent(int fd, bool pollin);

    int RemoveConsumer(int fd);

private:
    void RunEpollLoop();

    struct EventContext {
        InputCallback  input_cb  = nullptr;
        OutputCallback output_cb = nullptr;
        void*          user_data = nullptr;
    };

    int                 _epfd = -1;
    int                 _efd  = -1;  // eventfd for wake
    std::thread         _thread;
    std::atomic<bool>   _stop{false};
};

}  // namespace fast
