# brpc mutex：timedlock、pthread hook 与 FastPthreadMutex

记录日期：2026-10-06。依据本地 brpc 源码 HEAD `d688e7550be4b4c41b9a4dc55add2a2c75be1296`。

## 问题背景

问题：`HAS_PTHREAD_MUTEX_TIMEDLOCK` 有什么用？`sys_pthread_mutex_lock` / `sys_pthread_mutex_timedlock` 做什么？既然 `butil::Mutex` 可以测试 pthread mutex，为何还要这套实现？

回答的关键是区分三个层次：接口可用性、锁分析钩子、底层互斥算法。这套代码是 brpc 的运行时设施，并非为基准测试新增的锁。

## HAS_PTHREAD_MUTEX_TIMEDLOCK

brpc `src/butil/synchronization/lock.h` 在 `OS_LINUX && __USE_XOPEN2K` 时将其定义为 1，否则为 0。这是该版本的编译时可用性判断，不是性能优化开关，也不是是否启用超时等待的运行时配置。

- `pthread_mutex_lock`：等待获取锁。
- `pthread_mutex_timedlock`：带绝对截止时间的加锁。
- 内部共用函数按 `abstime == NULL` 调普通 lock，否则调 timedlock。

宏控制 timedlock 方法、函数指针及符号解析是否参与编译。之前的 `MutexTest.performance` 调用普通 `lock()`，没有测试 timedlock。

## sys_pthread_mutex_* 是什么

brpc `src/bthread/mutex.cpp` 中的这些变量是函数指针，不是新系统调用，也不是另一种锁算法。以普通 lock 为例：

```cpp
typedef int (*MutexOp)(pthread_mutex_t*);
static MutexOp sys_pthread_mutex_lock = first_sys_pthread_mutex_lock;
```

初始化通过 `_dl_sym` 或 `dlsym(RTLD_NEXT, "pthread_mutex_lock")` 查找下一层实现；正常情况下最终进入系统 pthread 库。初始化完成后指针直接指向解析结果，避免每次加锁都执行 `pthread_once`。源码还用全局初始化尝试在 main 前完成解析。

未定义 `NO_PTHREAD_MUTEX_HOOK` 时，brpc 拦截 pthread 标准函数入口，接入 contention profiler，以及按编译配置启用的 owner 检查、线程持锁计数等调试逻辑：

```text
butil::Mutex::lock()
  → pthread_mutex_lock（brpc hook）
    → pthread_mutex_lock_impl / pthread_mutex_lock_internal
      → sys_pthread_mutex_lock
        → 原生 pthread 实现
```

hook 内若再次直接调用被拦截的同名函数，会递归进入自身，因此必须保留通往下一层的入口。`sys_pthread_mutex_timedlock`、unlock 等同理。

初始化代码的复杂性还包含原版对动态符号解析期间分配内存、再次触发加锁的兼容处理，不能把这些分支理解成提升锁吞吐的算法。

## 为什么 butil::Mutex 不足以替代 hook

`butil::Mutex` 只是 C++ 封装；在 Linux 下，其 lock/unlock 调用 pthread API。若只在这个类中采样，就只能观察使用该类的代码。拦截 pthread 入口可以覆盖直接调用 pthread、以及经过该符号的第三方库调用。

因此：

| 组成 | 职责 |
|---|---|
| `butil::Mutex` | C++ 易用接口 |
| pthread hook | 竞争采样、可选调试 |
| `sys_pthread_mutex_*` | 绕过当前 hook，进入下一层实现 |
| `internal::FastPthreadMutex` | 另一套原子状态 + futex 互斥算法 |

仅测试普通 pthread 锁可以使用 `butil::Mutex`，不需要为测试专门实现 hook。已有 brpc 测试链接的是包含这些设施的库；关闭 profiler 会跳过采样，并不等于删除 hook。

## FastPthreadMutex 为什么也经过 pthread_mutex_lock_impl

该函数是共用模板，内部按锁类型选择不同重载：

```text
pthread_mutex_t*             → sys_pthread_mutex_lock → pthread mutex
internal::FastPthreadMutex*  → 自有 lock/try_lock     → 原子操作 + futex
```

原版开启 `BTHREAD_USE_FAST_PTHREAD_MUTEX` 时使用自有实现，否则内部类型回退成 `butil::Mutex`。外层 `bthread::FastPthreadMutex` 将它接入共用采样逻辑。函数名中包含 pthread，不意味着两条路径都调用原生 pthread mutex。

它的等待会阻塞 OS 线程。`bthread::Mutex` 则依赖 Butex，可在 bthread 上下文挂起任务。Butex 的等待队列锁不能替换成依赖自身的 bthread mutex。

## 基准应该怎样理解

数据与环境见 [brpc mutex 基准实测](brpc-mutex-benchmark.md)。测试对象是 `butil::Mutex`、`bthread::FastPthreadMutex`、`bthread::Mutex`，没有单独测试 Butex。

12 个执行者争抢同一把锁，临界区只递增自己的计数器，每组约 0.5 秒。7 轮中位数对应的近似吞吐：

| 锁 | pthread：百万次/秒 | bthread：百万次/秒 |
|---|---:|---:|
| `butil::Mutex` | 17.20 | 16.91 |
| `FastPthreadMutex` | 24.49 | 23.67 |
| `bthread::Mutex` | 25.62 | 25.58 |

FastPthreadMutex 吞吐分别高约 42.4% / 39.9%，不是单次加锁延迟下降相同比例，也不能把全部差异归因于某条原子操作或系统调用。

原版 `average_time = sum(执行者实际循环耗时) / sum(操作数)`。它不是统一墙钟耗时、CPU 时间或独立锁等待延迟。当 12 个 bthread 运行于默认 9 个 worker 时，阻塞 worker 的锁可能使部分任务延迟进入计时循环，其之前的调度等待不进入分子。因此 FastPthreadMutex 的 380 ns 小于 bthread::Mutex 的 468 ns，并不矛盾于后者吞吐更高。近似用 `9 × 0.5 秒 / 1183 万` 与 `12 × 0.5 秒 / 1279 万` 可解释这一口径差异，但不是对任务时序的实测还原。

这组数据来自 brpc 集成环境、WSL2、极短临界区；未测尾延迟、公平性或真实 RPC 收益，测试控制标志还有非原子访问。不能视为完全绕过 brpc 的纯 libc 基准或严格性能证明。

## light-rpc 的移植修正

此前 [mutex.h](../../src-bthread/mutex.h) 用 `std::mutex` 替代 `internal::FastPthreadMutex`，缺少 timed_lock；原版 FastPthreadMutex 的测试收益不能直接套到该替代实现上。

本次按用户要求将 [mutex.cpp](../../src-bthread/mutex.cpp) 改为原版启用分支：

- 保留 unsigned 锁字及 locked/contended 字节布局；复用已有 `MutexInternal` 和状态常量。
- try_lock 保留字节 exchange 的 acquire；竞争路径保留整字 exchange 的默认 seq_cst；unlock 保留整字 exchange 的 release。
- 竞争时直接 `futex_wait_private`，忽略 EWOULDBLOCK / EINTR 后重试；其他错误按原版返回 errno。普通 lock 按原版忽略慢路径返回值。
- unlock 根据原版 previous-state 判断是否 `futex_wake_private(..., 1)`，不改成 Butex 等待。
- 补齐内部及外部 timed_lock，保留绝对时间转相对 futex timeout 的算法。

适配范围：`bthread` 命名空间改为 `fast`；原版 `butil::atomic` 使用项目已有的 `std::atomic` 适配，保持原操作与内存序。Linux 固定使用 fast 分支，不添加 std::mutex 回退。保持现有裁剪：无 profiler、pthread hook、owner 跟踪及调度安全持锁计数，外层直接转发。

原版 timed_lock 有一个需要明确的边界：代码判断的是 `abstime_us > MIN_SLEEP_US`，不是剩余时间。对于已过期但仍大于该阈值的绝对时间，负相对 timeout 可能使 futex 返回 EINVAL。本次保留上游实际行为，未擅自改写为一律 ETIMEDOUT；这不应被描述为与 pthread timedlock 所有边界完全等价。无竞争时先尝试获取锁，成功则不检查截止时间。

锁存储布局发生变化，依赖头文件的对象必须重新编译。本次做功能回归，不声称移植后的性能已达到原版基准。

## 验证记录

- 新增 4 个 `FastPthreadMutexTest`：try_lock 竞争及空闲锁的过期截止时间、8 线程共享计数、等待超时后复用、解锁唤醒及受保护数据可见性。先确认旧实现因缺少 timed_lock 编译失败，再完成移植。
- `cmake --build /tmp/light-rpc-r04-debug -j 2` 全量构建通过。
- 主 `unit_tests` 的 116 个测试全部通过；单独运行 FastPthreadMutex / bthread 的 16 个测试也全部通过。
- 完整 CTest 为 5/6 通过。`RdmaWritableTests`（包含 ButexRuntime）和三组日志测试通过；`RdmaWriteQueueTests` 中 `ConcurrentSubmissionsDuringFailureReleaseEveryBufferOnce` 的 unexpected_error 断言间歇失败。
- 用修改前项目 HEAD `2702dd5cec0742a3fd6400af5629874ddc4aecc5` 的独立源码与 Debug 构建做对照，同一写队列用例重复 100 次失败 17 次，确认修改前已有该问题。本次未修改写队列或放宽断言，完整 CTest 不能宣称全绿。
- `git diff --check` 通过。未重新运行性能基准。

### 写队列失败用例的后续排查与修正

GDB 在 `ConcurrentSubmissionsDuringFailureReleaseEveryBufferOnce` 的错误计数分支捕获到线程 errno 为 ECANCELED（125），停下时对象的 `_write_error` 已为 EPIPE（32）。`SetFailedImpl` 先通过 CAS 发布失败版本，再执行 `OnFailed` 写入具体错误；并发读者在窗口内会由 `error()` 得到 ECANCELED 兜底。brpc `Socket::non_zero_error_code()` 明确允许同类窗口，以 EFAILEDSOCKET 兜底。加强内存序不能消除两个顺序动作之间的窗口。

经用户确认，仅修正测试契约：并发阶段接受 EPIPE / ECANCELED，其他错误仍失败；保留 513 个 buffer 恰好释放一次及队列清空检查；失败处理和线程 join 完成后，额外检查具体错误及后续写入均为 EPIPE，并验证新增被拒绝 buffer 的释放。未修改生产代码的失败发布顺序。

修正后全量编译通过，该并发用例重复 100 次全部通过，完整 CTest 6/6 通过。上面的 5/6 是修正测试前的历史结果。

## 自测问题

1. 为什么关闭 profiler 后仍可能经过 pthread hook？
2. 为什么 hook 内部不能直接再次调用 pthread_mutex_lock？
3. FastPthreadMutex 和 bthread::Mutex 的等待分别阻塞谁？
4. 为什么 bthread 测试的 average_time 不能直接取倒数得到吞吐？
