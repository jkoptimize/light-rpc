#ifndef FAST_BTHREAD_CONFIG_H
#define FAST_BTHREAD_CONFIG_H

#include <cstdint>  // uint32_t
#include <string>   // std::string

namespace fast {

// 在首次启动 bthread 前设置。当前字段不会自动校验，也不支持并发修改。
// 运行时调整线程数请使用 bthread_setconcurrency / bthread_setconcurrency_by_tag。
struct FastBthreadConfig {
    // worker 线程数量，默认 9，建议取值 4～1024。
    // min_concurrency 为 0 时，首次启动就创建这么多线程；
    // min_concurrency 大于 0 时，这是自动增加线程的上限。
    int bthread_concurrency      = 8 + 1;

    // 控制启动时创建多少 worker，以及是否按需增加线程。
    // 0（或负数）：一次创建 concurrency 个线程，关闭按需增加。
    // 正数：先创建这么多线程；唤醒空闲 worker 不足时，逐步增加到 concurrency。
    // 例如 min=4、concurrency=9：先启动 4 个，忙时最多增加到 9 个；空闲后不自动缩减。
    // 启用时至少设为 4，且不能大于 concurrency。每个 tag 至少需要一个初始 worker，
    // 例如分成 6 个 tag，初始线程数就至少要有 6 个。
    int bthread_min_concurrency  = 0;

    // 原版用于选择要调整线程数的 tag；-1 表示未选择。
    // 当前移植未使用此字段，修改无效。
    int bthread_current_tag      = -1;

    // 原版用于设置所选 tag 的线程数，默认 9。当前移植未使用此字段；
    // 调整某组线程数请调用 bthread_setconcurrency_by_tag。
    int bthread_concurrency_by_tag = 8 + 1;

    // 每个 worker 的本地任务队列容量，默认 4096；远程提交队列容量为其一半。
    // 取值必须是至少为 2 的 2 次幂，例如 1024、2048、4096。
    int task_group_runqueue_capacity = 4096;

    // 小栈大小：32 KiB，单位为字节。
    int stack_size_small   = 32768;

    // 默认栈大小：1 MiB，单位为字节；使用默认任务属性时选择此栈。
    int stack_size_normal  = 1048576;

    // 大栈大小：8 MiB，单位为字节；供需要更多栈空间的任务选择。
    int stack_size_large   = 8388608;

    // 栈保护区大小，默认 4 KiB，分配时按系统页对齐；访问保护区会触发错误。
    // 设为 0 或负数会关闭保护区，并改用 malloc 分配栈。
    int guard_page_size    = 4096;

    // 每个线程缓存空闲小栈的批次大小，默认 32 个；批次满后交回共享池。
    // 设为 0 不代表立即释放栈，通常保留正数。
    int tc_stack_small     = 32;

    // 每个线程缓存空闲默认栈的批次大小，默认 8 个；规则同 tc_stack_small。
    int tc_stack_normal    = 8;

    // worker 的分组数，默认 1，组编号从 0 开始。
    // 启动时把线程轮流分配到各组；组数至少为 1，且不能超过初始线程数。
    int task_group_ntags          = 1;

    // 每组的 ParkingLot 数量，默认 4，取值 4～1024。
    // 同组 worker 分散到这些等待设施上，减少等待和唤醒时的竞争。
    int parking_lot_of_each_tag   = 4;

    // TaskGroup 从列表移除后，延迟多少秒执行删除，默认 1 秒。
    // 给仍在访问它的线程留出时间，但固定延迟不能严格保证访问已结束。
    int task_group_delete_delay   = 1;

    // 是否给 worker 设置 brpc_wkr:<tag>-<序号> 形式的线程名，默认开启。
    bool task_group_set_worker_name = true;

    // worker 绑定的逻辑 CPU 编号，例如 "0-3,5,7"；空字符串表示不绑核。
    // 多个 worker 轮流使用列表中的 CPU。
    std::string cpu_set           = "";

    // 是否在没有等待线程时省略 futex 唤醒调用，默认关闭。
    // 开启可减少系统调用，但会增加等待线程数量的统计操作。
    bool parking_lot_no_signal_when_no_waiter = false;

    // 是否启用每组共享的优先任务队列，默认关闭。
    // 开启后，带 BTHREAD_GLOBAL_PRIORITY 的任务在 foreground 切换时可入该队列；
    // 其他 worker 窃取任务时优先检查它。
    bool enable_bthread_priority_queue = false;

    // 是否在任务切换时额外统计实际占用 CPU 的时间，默认关闭。
    // 开启会增加时钟读取开销。
    bool bthread_enable_cpu_clock_stat = false;

    // 每个线程缓存空闲 KeyTable 的数量阈值；KeyTable 保存任务的局部变量。
    // 超过默认 4000 张表时，将其中 2000 张交回共享池。取值应至少为 2。
    uint32_t key_table_list_size  = 4000;

    // 本地 KeyTable 用完时，一次从共享池补充的最多表数，默认 200。
    // 设为 0 会关闭这条批量补充路径。
    uint32_t borrow_from_globle_size = 200;

    // 定时任务的接收桶数量，默认 13，取值 1～1024。
    // 提交线程分散到不同桶，减少竞争；由一个 TimerThread 处理到期任务。
    uint32_t brpc_timer_num_buckets = 13;

    // 预留的定时粒度，单位微秒，1000 = 1 毫秒；当前未使用，修改无效。
    int timer_granularity_us      = 1000;

    // 返回全进程共用的配置对象。
    static FastBthreadConfig& Get() {
        static FastBthreadConfig config;
        return config;
    }
};

} // namespace fast
#endif
