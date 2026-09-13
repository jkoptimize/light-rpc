#ifndef FAST_BTHREAD_CONFIG_H
#define FAST_BTHREAD_CONFIG_H

#include <cstdint>  // uint32_t
#include <string>   // std::string

namespace fast {

struct FastBthreadConfig {
    int bthread_concurrency      = 8 + 1;
    int bthread_min_concurrency  = 0;
    int bthread_current_tag      = -1;   // BTHREAD_TAG_INVALID
    int bthread_concurrency_by_tag = 8 + 1;
    int task_group_runqueue_capacity = 4096;
    int stack_size_small   = 32768;
    int stack_size_normal  = 1048576;
    int stack_size_large   = 8388608;
    int guard_page_size    = 4096;
    int tc_stack_small     = 32;
    int tc_stack_normal    = 8;
    int task_group_ntags          = 1;
    int parking_lot_of_each_tag   = 4;
    int task_group_delete_delay   = 1;
    bool task_group_set_worker_name = true;
    std::string cpu_set           = "";
    bool parking_lot_no_signal_when_no_waiter = false;
    bool enable_bthread_priority_queue = false;
    bool bthread_enable_cpu_clock_stat = false;
    uint32_t key_table_list_size  = 4000;
    uint32_t borrow_from_globle_size = 200;
    uint32_t brpc_timer_num_buckets = 13;
    int timer_granularity_us      = 1000;

    static FastBthreadConfig& Get() {
        static FastBthreadConfig config;
        return config;
    }
};

} // namespace fast
#endif
