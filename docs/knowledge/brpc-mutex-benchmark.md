# brpc 原版 mutex 基准实测（2026-10-06）

## 范围与结论

运行本地 brpc 的 `bthread_mutex_unittest --gtest_filter=MutexTest.performance`，没有修改基准源码或锁实现。一次预热，随后 7 次独立进程运行；全部通过。

以每组约 0.5 秒内的操作数中位数比较，`FastPthreadMutex` 相对 `butil::Mutex` 在 pthread 场景高约 42.4%，在 bthread 场景高约 39.9%。`bthread::Mutex` 的操作数又分别比 FastPthreadMutex 高约 4.6% 和 8.1%。这是当前机器上高竞争、极短临界区的结果，不是所有负载的普遍排序，也不能据此将 Butex 内部锁换成依赖 Butex 的 bthread mutex。

## 环境

- CPU：Intel Core i5-1155G7，WSL2 向当前环境暴露 6 个逻辑 CPU，允许 CPU 0–5，未绑核。
- 内核：6.6.87.2-microsoft-standard-WSL2。
- GCC：11.4.0；CMake Release，实际最后生效优化为 `-O3 -DNDEBUG`。
- brpc 源码：`/home/syt/Desktop/brpc/brpc`；Git HEAD：`d688e7550be4b4c41b9a4dc55add2a2c75be1296`，测试后工作区干净。
- 编译时 BRPC_REVISION 字符串为 `1.15.0|main|2702dd5|2026-10-05T14:36:04+08:00`，与 Git HEAD 分开记录。
- 启用 `BTHREAD_USE_FAST_PTHREAD_MUTEX`；锁调试、调度安全调试、RDMA、CPU profiler、contention profiler 均未启用。
- 测试链接的库名为 `brpc-shared-debug`，但其实际编译也使用上述优化；名字不代表未优化构建。
- bthread 默认并发数 9（`8 + BTHREAD_EPOLL_THREAD_NUM`，后者为 1）；每组创建 12 个 pthread 或 bthread。
- 编译完成后才运行测试。依赖只下载并解包到 `/tmp`，没有安装系统软件包。

## 负载及指标

三种锁：`butil::Mutex`（pthread mutex 封装）、`bthread::FastPthreadMutex`、`bthread::Mutex`（bthread_mutex_t 封装）。没有单独测试 std::mutex。

所有执行者争抢同一把锁，锁内只递增各自的计数器。主线程置启动标志，睡眠 500ms 后置停止标志。每轮六组顺序固定；正式轮次间等待 1 秒。

- 操作数：12 个执行者计数之和；本报告用其中位数比较。
- 近似吞吐：操作数除以名义 0.5 秒，单位百万次/秒；不是额外精确测量的墙钟吞吐。
- 原版 average_time：`sum(每个执行者实际循环耗时_ns) / sum(操作数)`，不是孤立 lock 指令耗时，也不是严格的锁等待延迟。
- 阻塞 worker 的锁会使部分 bthread 在启动后延迟进入计时循环。原版 average_time 因此不适合直接跨三种锁的 bthread 场景比较，更不能简单用它的倒数当吞吐。

| 锁 | 执行上下文 | 操作数中位数 | 7 轮操作数范围 | 近似吞吐（百万次/秒） | 原版 average_time 中位数（ns） |
|---|---|---:|---:|---:|---:|
| `butil::Mutex` | pthread | 8,599,947 | 8,066,955–9,288,579 | 17.20 | 696.6 |
| `butil::Mutex` | bthread | 8,456,679 | 7,768,449–9,012,541 | 16.91 | 531.9 |
| `bthread::FastPthreadMutex` | pthread | 12,246,295 | 11,134,058–12,677,954 | 24.49 | 488.9 |
| `bthread::FastPthreadMutex` | bthread | 11,834,555 | 10,819,777–12,746,207 | 23.67 | 380.0 |
| `bthread::Mutex` | pthread | 12,807,753 | 12,463,383–13,307,133 | 25.62 | 468.0 |
| `bthread::Mutex` | bthread | 12,791,460 | 11,863,476–13,557,874 | 25.58 | 468.3 |

## 局限

这是 12 个执行者争抢单锁的竞争基准，不测无竞争延迟，也不模拟真实 Butex 的持锁操作。虚拟化、宿主机负载、频率变化及固定测试顺序均会影响结果；没有测尾延迟、公平性或置信区间。原版基准的 `g_started`、`g_stopped`、`ready` 是非原子 bool，存在 C++ 层面的并发数据竞争；为保留原测试本次未修改，所以这些数字应当作为初步实测，不能作为严格性能证明。

## 复现

构建产物：`/tmp/brpc-mutex-bench/test/bthread_mutex_unittest`。

依赖准备（已执行；只解包，不安装）：

```bash
mkdir -p /tmp/brpc-mutex-deps
cd /tmp/brpc-mutex-deps
apt-get download libgflags-dev libgflags2.2 libleveldb-dev libleveldb1d libprotoc-dev
for pkg in /tmp/brpc-mutex-deps/*.deb; do
    dpkg-deb -x "$pkg" /tmp/brpc-mutex-deps/root
done
```

系统已有 protobuf、OpenSSL、googletest 等依赖。配置及构建：

```bash
cmake -S /home/syt/Desktop/brpc/brpc -B /tmp/brpc-mutex-bench   -DCMAKE_BUILD_TYPE=Release -DBUILD_UNIT_TESTS=ON   -DDOWNLOAD_GTEST=OFF -DBRPC_SYSTEM_GTEST_SOURCE_DIR=/usr/src/googletest   -DBUILD_BRPC_TOOLS=OFF -DCMAKE_PREFIX_PATH=/tmp/brpc-mutex-deps/root/usr   -DGPERFTOOLS_INCLUDE_DIR=/usr/include -DGPERFTOOLS_TCMALLOC_AND_PROFILER=''
cmake --build /tmp/brpc-mutex-bench --target bthread_mutex_unittest -j 2
```

运行命令（一次预热后执行 7 次，每次独立进程）：

```bash
LD_LIBRARY_PATH=/tmp/brpc-mutex-deps/root/usr/lib/x86_64-linux-gnu   /tmp/brpc-mutex-bench/test/bthread_mutex_unittest   --gtest_filter=MutexTest.performance
```

原始数据：[results.json](benchmarks/brpc-mutex-2026-10-06/results.json)。

日志：
- [run-1.log](benchmarks/brpc-mutex-2026-10-06/run-1.log)
- [run-2.log](benchmarks/brpc-mutex-2026-10-06/run-2.log)
- [run-3.log](benchmarks/brpc-mutex-2026-10-06/run-3.log)
- [run-4.log](benchmarks/brpc-mutex-2026-10-06/run-4.log)
- [run-5.log](benchmarks/brpc-mutex-2026-10-06/run-5.log)
- [run-6.log](benchmarks/brpc-mutex-2026-10-06/run-6.log)
- [run-7.log](benchmarks/brpc-mutex-2026-10-06/run-7.log)
- [warmup.log](benchmarks/brpc-mutex-2026-10-06/warmup.log)

源码入口：brpc `test/bthread_mutex_unittest.cpp` 的 `PerfTest`、`add_with_mutex` 和 `MutexTest.performance`。
