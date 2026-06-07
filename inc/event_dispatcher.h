#pragma once

#include <atomic>
#include <thread>
#include <unordered_map>
#include <cstdint>

namespace fast {

// Epoll-based event dispatcher, one global instance.
// Thread auto-started in constructor.
//
// RegisterEvent:   EPOLL_CTL_ADD with caller-specified event mask.
// UnregisterEvent: EPOLL_CTL_DEL and free the associated EventContext.

class EventDispatcher {
public:
    using InputCallback  = void (*)(void* user_data, uint32_t events);
    using OutputCallback = void (*)(void* user_data, uint32_t events);

    static EventDispatcher& GetInstance();

    EventDispatcher();
    ~EventDispatcher();

    int RegisterEvent(int fd, InputCallback in_cb, OutputCallback out_cb,
                       void* user_data, uint32_t events);

    int UnregisterEvent(int fd);

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
    std::unordered_map<int, EventContext*> _fd_map;
};

}  // namespace fast
