# bthread 基础设施移植复核（2026-10-06）

## 结论与适用范围

对照本地 brpc HEAD `d688e7550be4b4c41b9a4dc55add2a2c75be1296`，复核 light-rpc HEAD `2702dd5cec0742a3fd6400af5629874ddc4aecc5` 加当前工作区改动，包括此前已完成的 FastPthreadMutex 移植。实际运行环境为 Linux / WSL2、x86_64、GCC 11.4、C++17。

当前已移植的调度、Butex、同步、ID、TLS、池和定时器核心，在默认及本次测试的合法配置下，未发现需要改写原算法的移植错误。发现并修复一项确定的移植遗漏：gflags 裁剪同时删除了启动配置校验；另外补齐配置拒绝后的 tagged concurrency 入口空指针处理。Debug / Release 全量构建及各自 7 组 CTest 均通过。

这一结论不等于移植了 brpc 的全部 bthread 功能，也不等于所有平台、故障注入和性能场景均已验证。审查中保留原版同步协议，不因理论上的“更安全”而增加原子操作、加强内存序或改写调度。

## 比对方法

- 比对 `src-bthread/` 顶层 41 个 `.h/.cpp` 与 brpc 对应文件，另直接比较 `offset_inl.list`，后者逐字节相同。
- 比对 `src-bthread/butil/` 下 30 个头文件／源文件，按扁平化目录映射回 brpc 的 containers、memory 等目录。
- 先排除注释、include 路径、命名空间、配置字段名、原子类型拼写差异，再逐项阅读剩余差异。归一化 diff 只是定位工具，不能单独证明行为等价；头文件依赖、编译条件及配置范围另行检查。
- 核对核心构建清单，实际编译所有目标；用 `nm` 核对公开 C API 与静态库符号。
- 增加独立链接 `fast_bthread` 的测试目标，避免只靠 RPC 目标的间接调用验证运行时。

## 核心模块比对结果

| 模块 | 核对重点与结果 |
|---|---|
| `context.*`、`processor.h`、`prime_offset.h` | 去除路径与命名空间适配后主体一致；x86_64 上实际执行栈切换，未运行 aarch64 验证 |
| `work_stealing_queue.h`、`task_group_inl.h` | 主体一致，包括最后一个任务的 pop/steal CAS 竞争、fence 与内存序 |
| `task_group.*`、`task_meta.h` | remained 回调、结束后回收、版本更新、任务迁移后重取 TLS、errno 保存恢复沿用原版；差异主要为 bvar/tracer 依赖裁剪 |
| `task_control.*`、`parking_lot.h`、`remote_task_queue.h` | worker 创建／唤醒／停止、tag 分组、停车状态和队列主线保留；pthread 类型别名及配置入口有适配，见下文 |
| `butex.*` | 初始比较、入队时二次比较、等待者锁、超时与中断协调、requeue、wake/wake_n 主体一致；Butex 的 ASan-poison 禁用特化仍保留 |
| `mutex.*` | 非 profiler 的 bthread mutex 算法保留；FastPthreadMutex 已使用原版 unsigned 锁字、字节 acquire exchange、竞争整字 exchange、release unlock 与 futex wait/wake |
| `condition_variable.*` | 主体一致，包含释放 mutex 前记录序列、恢复后的重新加锁以及 broadcast requeue |
| `semaphore.cpp`、`rwlock.*` | 差异为 contention profiler 裁剪及必要类型适配；计数、等待与唤醒原子操作未改动 |
| `bthread_once.cpp` | 初始化状态竞争、等待及唤醒主体一致 |
| `id.*`、`list_of_abafree_id.h` | 主体一致；版本号、排队错误、延迟回调、销毁与等待协议保留 |
| `key.cpp` | KeyTable/子表、版本化 key、析构与复用流程保留；主要差异为配置来源、原子初始化及 bvar 移除 |
| `stack.*`、`stack_inl.h` | 栈池、guard page、malloc/mmap 分支和上下文创建保留；Valgrind 注册裁剪 |
| `timer_thread.*` | 分桶提交、最近期限、版本化撤销和执行协调保留；线程命名及统计导出有裁剪／适配 |
| `interrupt_pthread.*`、`sys_futex.*`、`errno.*` | 运行时主体保留；自定义错误字符串注册被裁剪，ESTOP 与 errno 访问保留 |

FastPthreadMutex 的详细分层与边界说明见 [mutex 分层与移植说明](brpc-mutex-hooks.md)。本次未重新修改其实现。

## 已修复：启动校验随 gflags 一起丢失

brpc `bthread.cpp` 的 validator 不只是配置说明：concurrency 通过 `bthread_setconcurrency` 校验，正数 min_concurrency 有范围约束，parking-lot 数量有上下界。旧移植只留下可直接赋值的 `FastBthreadConfig`，初始化前没有相同校验。

修复前实测：

1. 设置 `parking_lot_of_each_tag=0` 后首次启动 bthread，子进程被 SIGFPE 终止。`TaskControl::_add_group` 中按 `_pl_num_of_each_tag` 取模，零值可到达该路径。
2. 正数 `bthread_min_concurrency=3` 被接受，而原版下界为 `BTHREAD_MIN_CONCURRENCY=4`；测试预期拒绝但实际创建成功。

修复在 [bthread.cpp](../../src-bthread/bthread.cpp) 的首次创建路径完成：

- `bthread_concurrency` 必须在 `[BTHREAD_MIN_CONCURRENCY, BTHREAD_MAX_CONCURRENCY]`，当前为 `[4, 1024]`。
- 正数 `bthread_min_concurrency` 必须在 `[4, bthread_concurrency]`；非正数仍按原版禁用惰性扩容。
- `parking_lot_of_each_tag` 必须在 `[BTHREAD_MIN_PARKINGLOT, BTHREAD_MAX_PARKINGLOT]`，当前为 `[4, 1024]`。
- 检查置于创建 TaskControl 前，拒绝时不启动 worker、不发布全局指针，修正配置后可以重试。

这里恢复的是原版已有约束。配置入口从 gflags 改为启动前 C++ 配置对象，因此验证时机改为首次创建调度器；不恢复 gflags 运行时赋值接口。正常调度热路径没有新增检查，未做性能基准，不能据此宣称性能完全相等。

`get_or_new_task_control()` 失败仍返回 NULL；start 等已有入口据此返回 ENOMEM，配置拒绝沿用这个错误映射，并非断言发生了内存耗尽。`bthread_setconcurrency_by_tag()` 之前没有检查 NULL；在新增拒绝路径下会解引用空指针，因此同时补齐相同错误返回。未改动成功后的扩容算法、锁或内存序。

## 依赖适配与有意裁剪

- ResourcePool/ObjectPool 保留 TLS 缓存、free chunk、全局 block group、地址／ID 映射和回收协议。`butil::atomic` 映射到 `std::atomic`，静态零初始化及 block 指针数组初始化做 C++17 适配。
- `butil/thread_local`、`thread_key` 保留原版 volatile-TLS 访问辅助函数、退出回调与版本协议，未机械替换为普通 C++ thread_local。
- FlatMap 保留 Butex 使用的普通整型 key map、桶内首节点、链表冲突与节点池；Sparse/Multi、字符串异构查找等不在当前移植范围。不能宣称此版本是完整 butil FlatMap API。
- fast_rand 保留调度使用的 xorshift128+、splitmix64 和范围采样；bytes/double 等接口未移植。time 保留开源默认的 CLOCK_MONOTONIC 路径，未移植内部 TSC 分支。
- `butil::Mutex` 在远程队列等处适配为 `std::mutex`；二者均阻塞 pthread，不能把此类适配推广成“可以替换 bthread mutex”。FastPthreadMutex 使用专用 futex 实现。异常／调试路径及性能不声称逐点完全等同。
- `pthread_numeric_id()` 在哈希分桶位置改为 Linux 的 `pthread_self()`，线程命名使用 `pthread_setname_np`；只按当前 Linux 范围判定。
- bvar、contention profiler、pthread hook、owner 调试、Valgrind 可按规则裁剪。未引入新的共享原子统计；仍保留的 stack/key 等原版统计更新未作为本次正确性修复顺带优化。

## 接口和配置边界

对 `bthread.h` 的 67 个 extern C 声明检查静态库符号，发现以下 7 个无定义：

- `bthread_barrier_init/destroy/wait`。
- `bthread_rwlockattr_init/destroy/getkind_np/setkind_np`。

在对照的 brpc `src/` 中，它们也只有声明、没有对应实现，不能算作本次移植丢失的源文件。当前不得使用这些入口；本次不编造空实现，也不把它们计入已验证能力。

FD/epoll 协程等待、execution_queue 等尚未移植；TaskControl 去掉 `stop_and_join_epoll_threads()` 与未接入 bthread FD 子系统相对应。项目自己的 EventDispatcher 不等于已经移植 bthread FD API。本次不增加 polling-CQ、短连接或连接池，也未修改 endpoint/连接生命周期。

配置仍须在首次启动前完成；不能并发直接修改结构字段。`current_tag`/`concurrency_by_tag` 配置字段仍无动态触发作用，运行时调整使用现有 setter。tag 数量与初始 worker 数、队列容量等仍须满足头文件记录的前提：例如 tag 数量为 0 时的取模错误、tag 数超过初始 worker 导致初始化等待、非法 runqueue 导致 worker 初始化失败，在该 brpc 基线也存在相同输入前提；本次没有把上游输入边界问题当成移植遗漏擅自改写。

此前讨论的 `mutex_lock_contended_impl` 以 `errno==0` 更新 first_wait，以及 FastPthreadMutex 对过期时间的部分 EINVAL 行为，同样继承自本地 brpc，未在移植复核中改动。源码相同不等于证明上游不存在问题。

## 验证

新增 [bthread_runtime_test.cc](../../test/bthread_runtime_test.cc)，只链接 fast_bthread、GTest、pthread，不依赖 RPC/RDMA 库。10 个用例覆盖：

1. parking-lot 数量范围拒绝。
2. concurrency 范围拒绝。
3. 正数 min_concurrency 范围拒绝。
4. tagged setter 拒绝非法启动、修正配置后重试成功。
5. 合法惰性启动、两个 tag、启用可选队列／停车配置下的基本提交与停止。
6. 32 个 bthread 的 once、TLS 跨 yield/sleep 保持、errno 跨 yield 保持与 key 析构。
7. 8 个条件变量等待者的 broadcast、requeue、mutex 重获取。
8. Butex 等待被 bthread_interrupt 中断并 join。
9. ID 锁持有期间错误排队，unlock 后回调销毁，拒绝过期 ID。
10. 独立 TimerThread 的取消与定时执行。

启动用例在子进程运行，以隔离全局调度器；alarm 限制故障场景运行时间。测试不声称强制发生了每一种 worker 迁移或所有可选优先级队列时序。

```bash
cmake -S . -B /tmp/light-rpc-r04-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/light-rpc-r04-debug -j 2
ctest --test-dir /tmp/light-rpc-r04-debug --output-on-failure --timeout 30

cmake -S . -B /tmp/light-rpc-bthread-audit-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/light-rpc-bthread-audit-release -j 2
ctest --test-dir /tmp/light-rpc-bthread-audit-release --output-on-failure --timeout 30
```

两种构建均完成，CTest 各 7/7 通过，包括主单元测试、写队列、ButexRuntime 及新增运行时测试。Release 构建另有现存 EventDispatcher 忽略 read 返回值的警告，本次未扩展修改。

未运行 aarch64、ASan/TSan/tracer 配置、RDMA 硬件集成或吞吐基准，不对这些范围作正确性／性能保证。已裁剪的诊断宏也不能直接当作当前支持的构建选项启用。

## 历史审查对照

[2026-09-25 记录](bthread-port-review.md) 中的核心未接入构建、缺失 interrupt_pthread、缺失 semaphore/rwlock、错误头文件路径等已被此前工作补齐；本次通过实际构建再次确认。FastPthreadMutex 的 std::mutex 替代已在前一轮恢复。本轮补上遗漏的启动校验，旧记录保留为历史，不应继续将其中已解决项当成当前故障。
