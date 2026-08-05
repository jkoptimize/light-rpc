#ifndef FAST_BTHREAD_CONFIG_H
#define FAST_BTHREAD_CONFIG_H

namespace fast {

struct FastBthreadConfig {
    int bthread_concurrency      = 8 + 1;
    int bthread_min_concurrency  = 0;
    int task_group_runqueue_capacity = 4096;
    int stack_size_small   = 32768;
    int stack_size_normal  = 1048576;
    int stack_size_large   = 8388608;
    int guard_page_size    = 4096;
    int tc_stack_small     = 32;
    int tc_stack_normal    = 8;
    int task_group_ntags          = 1;
    int parking_lot_of_each_tag   = 4;
    int timer_granularity_us      = 1000;

    static FastBthreadConfig& Get() {
        static FastBthreadConfig config;
        return config;
    }
};

} // namespace fast
#endif
