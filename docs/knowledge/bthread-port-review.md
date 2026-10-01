# bthread 移植偏差检查（mutex 之外）

> 检查日期：2026-09-25。移植基线：当前 HEAD `27dd54b`；对照 `/home/syt/Desktop/brpc/brpc/src/bthread/` 及其 butil/bvar 依赖。
> 本次更新学习文档并审查源码，不修改运行时代码。以本项目移植代码为学习对象。

## 1. 判断原则与结论

用户原则：维测性质的 profiler/debug/tracer 可以裁剪；基础能力尽量保留，只有在 C++ 标准设施确实能承担相同语义时才替换，并尽量保持原生性能。IOBuf 复用 `src-common/` 与 `inc/fast_iobuf.h`，不重复移植。

当前主要调度、butex、栈、TLS、池和队列算法仍接近原版，未发现将它们整体改写为另一套调度模型的情况。但即使不计 mutex，仍存在编译接入缺口、基础依赖缺失、配置行为变化和热点性能风险，不能认定已完成等价移植。

下面区分「已确认的代码/构建事实」与「尚未实测的性能风险」。这不是完整的线程安全证明，也不是性能基准结论。

## 2. 已确认的构建与基础能力缺口

> 2026-09-27 后续更新：已将 src-bthread 搜索路径改为 `-iquote`，解决本目录 errno.h 遮蔽系统头的问题。现有 fast_bthread 基础库目标可编译；下述核心接入、缺失依赖等问题仍待处理。详情见 [日志与头文件搜索修复记录](bthread-logging-port.md)。

### 2.1 核心未进入构建，路径适配尚未完成

`src-bthread/CMakeLists.txt` 仅包含 time、fast_rand、thread_local、thread_key、murmurhash3 五个 butil cpp，尚未编译核心。

实际仍有旧目录引用：

- `remote_task_queue.h` 引用不存在的 `butil/containers/bounded_queue.h`。
- `butex.cpp` 引用不存在的 `butil/containers/flat_map.h`、`linked_list.h`。
- 对应容器已在 `src-bthread/butil/`，应修正路径，不重复移植。

这是接入未完成，不是应当学习或保留的算法差异。

### 2.2 errno 不能按「berror 已替换」直接删除

`src-bthread/errno.cpp` 还定义 `ESTOP` 和 `bthread_errno_location()`；`errno.h` 用后者适配 errno。`BAIDU_REGISTER_ERRNO` 的原依赖已移除，但宏调用残留。

另外，当前 CMake 将 `src-bthread` 放入 `-I` 搜索路径，其同名 `errno.h` 会遮蔽系统 `<errno.h>`。直接语法检查已复现标准库中 errno / ERANGE 等未定义的错误。

建议采用不会遮蔽系统头的布局或头文件名，并明确保留错误码与 errno 访问语义。允许移除错误字符串注册，但要与运行时基础定义分开。原实现计划 Task 14「直接删除 errno.cpp」应修正，不能照做。

### 2.3 配置头与日志/原子宏替换尚未闭合

- 多个核心文件使用 `FastBthreadConfig`，但 `src-bthread/` 中未找到对 `inc/fast_bthread_config.h` 的 include；现有目标也没有强制包含它。
- `timer_thread.cpp`、`condition_variable.h`、`key.cpp` 等仍使用未提供的 `CHECK_EQ`。
- `TaskControl::choose_one_group` 仍有 `CHECK(false) << ...`，与项目 do/while 风格的 CHECK 不兼容。
- `key.cpp` 的计数器保留 `BUTIL_STATIC_ATOMIC_INIT`，定义已不在现有宏层。

修复这些适配时应保留必要表达式的执行。比如 `CHECK_EQ(0, stop_and_join_epoll_threads())` 包含真实操作，不能为了删检查而把操作也删掉。

### 2.4 worker 停止所需的中断设施缺失

`task_control.cpp` 无条件 include `interrupt_pthread.h`，默认非 tracer 分支在 `stop_and_join()` 中调用 `interrupt_pthread(worker)`，但该头和实现未移植。

原版实现通过注册信号处理器并发送信号，尝试打断可中断的阻塞调用。这属于线程生命周期支持，不能按 profiler/debug 一起丢弃，也不代表能打断所有阻塞行为。按当前原则应保留或提供等价实现，并明确其信号约定。

同一停止路径调用 `stop_and_join_epoll_threads()`，但当前没有对应定义。若不移植 bthread FD/epoll 子系统，应成套处理启动、等待、停止依赖；不能留悬空符号或用无依据的空实现掩盖问题。

### 2.5 公开接口与已移植模块不一致

`bthread.h` 保留了 semaphore 和 rwlock API，`types.h` 也保留相关类型，但没有 `semaphore.cpp` / `rwlock.cpp`。

这些是同步能力，不属于维测代码。按本次明确的原则，建议排入后续移植；第一轮学习仍可以延后。C++17 的线程同步设施不能直接代替它们的协程等待语义。

FD/epoll、execution_queue、countdown_event 等属于功能范围选择，不能仅因不是最小调度内核就称为 debug。它们可以分批移植，但应注明未支持，处理关联依赖，并让头文件声明与实际能力一致。

## 3. 行为和性能方面的偏差

### 3.1 gflags 替换删掉了功能校验与动态更新行为

`bthread.cpp` 删除的 validator 不只是参数描述：

- parking lot 数量的上下界检查；
- min_concurrency 的合法性检查及按需增加 worker；
- current_tag 的合法性、关联并发度更新；
- 配置更新触发的 concurrency setter。

现在是公开可变的 `FastBthreadConfig` 字段，直接赋值不会执行上述行为。`TaskControl` 用配置构造数组，并在多处读取 tag 数；直接运行时修改也缺少统一同步和结构更新协议。

具体例子：`parking_lot_of_each_tag = 0` 不再被旧 validator 拒绝，而 group 分配 parking lot / signal 路径有对该值取模的操作。

建议：启动前统一验证并固定结构性配置；运行时调参通过明确的 setter 执行同步和副作用。不能把「去掉 gflags 依赖」等同于「去掉校验与更新协议」。TimerThread 自己的 bucket 数量检查仍保留，不应说所有校验都被删除。

### 3.2 bvar 裁剪引入了任务热点共享原子计数

> 2026-09-27 后续更新：经确认，已删除当前无消费者的 `_nbthreads`、`_nworkers` 两个字段及全部五处更新。原版中它们分别导出 `bthread_count`、`bthread_worker_count` 统计指标；参与 worker 扩容判断的 `_concurrency` 保留。以下保留最初审计事实。

`task_group.cpp` 的任务创建/结束把 `_nbthreads << ±1` 换成同一个 `TaskControl::_nbthreads` 上的 relaxed fetch_add/sub。

原版常规 bvar Reducer 更新路径通过 `get_or_create_tls_agent()` 分散写入；当前让多个 worker 在同一原子变量上竞争。relaxed 降低排序约束，但不消除共享缓存行争用。

仓库中这两个计数器 `_nbthreads`、`_nworkers` 当前只看到定义与增减，没有消费者。建议无监控需求时删除无用计数；需要统计时沿用每 worker/TLS 分散累计方式。不要为了剥离监控反而增加共享热点。

这是明确的更新结构变化；吞吐或延迟损失尚未通过基准量化。`_nworkers` 只在 worker 生命周期更新，重点是高频 `_nbthreads`。

### 3.3 日志限频丢失，部分 DCHECK 被提升为 CHECK

> 2026-09-27 更新：本节所列限频与 DCHECK 偏差已修复，并补齐日志宏惰性求值与独立测试，见 [日志基础设施修复](bthread-logging-port.md)。以下保留最初审计事实；bthread 整体编译缺口仍未解决。

- `task_group_inl.h::push_rq` 与 `task_group.cpp::ready_to_run_remote` 将 `LOG_EVERY_SECOND` 改为每次循环输出。
- 队列满时循环还会 `usleep(1000)`，多个线程持续输出可能放大过载；项目 LogMessage 使用字符串流并在输出时 flush。
- `run_main_task` 中原来的 DCHECK 被改为发布版本也执行的 CHECK。

建议保留低开销限频机制；调试断言保持 debug 条件，运行时必要检查保留。不要把所有检查都关闭，尤其不能删除其中承担实际工作的函数调用。

日志引起的性能影响集中于相应触发路径，当前未做压测，不能推断正常负载必然明显退化。

### 3.4 基础工具仍采用「只保留当前使用部分」的裁剪范围

例如 fast_rand 保留 xorshift128+、splitmix64、TLS seed 与范围采样核心，但删去了 `fast_rand_in_64/u64`、double、bytes 等基础接口。当前已检查的调度调用未发现需要这些被删接口；这属于接口范围缩减，并非已证实的调度错误。

若严格采用「基础尽量保留」原则，后续应恢复需要兼容的基础 API，或明确支持范围。不要用 std 随机引擎替换现有热点生成器来填补接口。

## 4. 目前符合原则的部分

- `task_group` / `task_group_inl` 的 remained、任务迁移后 TLS 重取、版本更新和回收主线保留。
- work-stealing 的原子操作和内存序、远程队列协议、butex 的二次检查和等待/唤醒主线未见整体改写。
- 栈池、对象池、资源池的 TLS 缓存及全局分块结构保留，未替换为逐任务 new/delete 或通用 STL 容器。
- `thread_local` 的迁移相关访问宏、thread_atexit 回调机制与 thread_key 版本协议基本沿用原版。
- fast_rand 热点生成算法保留。
- `cpuwide_time_ns()` 的 CLOCK_MONOTONIC 路径与原版非 BAIDU_INTERNAL 分支一致；不能把删掉内部 TSC 分支称作相对开源默认实现的性能退化。
- bvar expose、tracer、valgrind 注解等维测依赖可以移除。

`std::atomic` 替换是合理方向，但仍需逐处确认初始化、布局、lock-free 要求与内存序。`std::mutex` 可承担内部 pthread 互斥用途，但不能据此替换会挂起协程的 bthread mutex/cond。TLS 析构顺序与迁移访问也不能仅凭 C++ 有 thread_local 就机械替换。

## 5. 已做的验证与建议次序

本次对核心文件执行直接语法检查：

```bash
g++ -std=c++17 -D_GNU_SOURCE -I. -Isrc-bthread -fsyntax-only src-bthread/<file>.cpp
```

抽查 bthread、task_group、butex、stack、timer_thread、condition_variable、bthread_once、context 共 8 个翻译单元。只有 context.cpp 通过，其余被缺失头文件或 errno 等适配错误阻塞。该检查使用当前 include 布局，不代表项目已有核心构建目标；也不是汇编执行、链接或并发行为验证。

建议修复顺序：

1. 修正头文件布局与路径、errno、配置 include、残留宏。
2. 补齐中断/停止基础依赖，明确非维测功能的移植清单。
3. 恢复配置校验和动态更新约定；清理统计引入的共享热点，恢复日志限频。
4. 完成 mutex 后把全部核心加入构建，验证链接与公开 API 一致性。
5. 在本项目执行 start/join、yield、sleep、TLS、butex、同步和停止流程实验；再在同机同参数下比较多 worker 创建/销毁、切换和唤醒的性能。

学习可以先按 [学习计划](bthread-learning-plan.md) 阶段 ② 跟踪接口路径；遇到这里记录的适配缺口，不必将其误解为原算法的复杂性。
