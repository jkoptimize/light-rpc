# butil/time.h + time.cpp — 时间测量工具集

## 1. 作用概述

`butil/time.h` 是 bthread 使用的**纳秒级时间测量工具集**，提供三类能力：

1. **读时钟**：`cpuwide_time_ns()`（单调墙上时钟，基于 `CLOCK_MONOTONIC`）、`gettimeofday_us()`（墙上时钟，基于 `gettimeofday`）、`monotonic_time_ns()`、`cputhread_time_ns()`（线程 CPU 时钟）。
2. **timespec / timeval 换算**：纳秒/微秒/毫秒/秒与 `timespec`/`timeval` 结构之间的互转，以及「从某时刻/从现在往后偏移」的算术。
3. **计时工具**：`Timer` 类（start/stop/elapsed），用于测量一段代码耗时。

这些函数以 `inline` 为主、直接系统调用、无内存分配，保证纳秒级开销，是调度器和定时器热路径的计时基础。

## 2. 关键 API

| 符号 | 签名 | 说明 |
|------|------|------|
| `cpuwide_time_ns()` | `int64_t` | **核心**。单调墙上时钟，`clock_gettime(CLOCK_MONOTONIC)`，调度计时基准 |
| `cpuwide_time_us/ms/s()` | `int64_t` | `cpuwide_time_ns` 的微/毫/秒变体 |
| `gettimeofday_us()` | `int64_t` | 墙上时钟微秒，用于定时器到期判断、超时换算 |
| `monotonic_time_ns()` | `int64_t` | `CLOCK_MONOTONIC` 的 out-of-line 实现（保持 ABI） |
| `cputhread_time_ns()` | `int64_t` | 线程 CPU 时间（`CLOCK_THREAD_CPUTIME_ID`） |
| `timespec_to_microseconds(ts)` | `int64_t` | `timespec` → 微秒整数 |
| `microseconds_to_timespec(us)` | `timespec` | 微秒整数 → `timespec` |
| `microseconds_from_now(us)` | `timespec` | 当前时间（`CLOCK_REALTIME`）+ 偏移 |
| `timespec_from_now(span)` | `timespec` | 当前时间 + `timespec` 偏移 |
| `timespec_add/minus/normalize` | — | `timespec` 算术与归一化（`tv_nsec` 归到 [0, 1e9)） |
| `Timer` | 类 | `start()`/`stop()`/`n_elapsed()`/`u_elapsed()` 等，测一段代码耗时 |

## 3. 在 bthread 中的作用

时间测量贯穿 bthread 调度器的多个热路径：

### 3.1 定时器线程 `timer_thread.cpp`
`gettimeofday_us()` 是定时器线程的**绝对时间基准**：
- 计算每个 `TimerTask` 是否到期：`if (gettimeofday_us() < task->run_time) { /* not ready */ }`
- 计算下次需要 sleep 的时长：`last_sleep_time = gettimeofday_us()`
- 支撑 `bthread_usleep()` / `bthread_timer_add()` 等上层 API 的定时唤醒

### 3.2 同步原语 `butex.cpp` / `mutex.cpp`
- `butex_wait` 带超时（`abstime`）时，用 `timespec_to_microseconds(*abstime) - gettimeofday_us()` 把绝对时间换算成**相对剩余微秒**，再转成 futex 等待参数。
- 超时/计时锁的实现依赖微秒精度的 `gettimeofday_us` 与 `timespec` 换算。

### 3.3 调度器统计 `task_control.cpp`
`cpuwide_time_ns()` 用于统计 worker 线程的**累计运行时间**（`_cumulated_worker_time`），以及每次上下文切换的耗时；`cputhread_time_ns()` 用于区分「真正消耗 CPU 的时间」和「阻塞等待的时间」。

### 3.4 通用计时
`Timer` 是测量任意代码段耗时的标准工具（原始 bthread 中 `task_tracer` 用它做 trace 计时）。

## 4. 移植裁剪决策

| 原文件内容 | 决策 | 理由 |
|------------|------|------|
| `NO_CLOCK_GETTIME_IN_MAC` 分支（mach 时钟） | **删除** | 仅 macOS < 10.12，目标平台 Linux |
| `last_changed_revision()` | **删除** | 读 SVN 版本，与时间无关，bthread 不用 |
| `BAIDU_INTERNAL` 的 `cpuwide_time_ns`（rdtsc + CPU 频率） | **删除** | 依赖 `detail::clock_cycles()`（x86/arm 汇编）和 `read_invariant_cpu_frequency()`（读 `/proc/cpuinfo`），默认本就不启用，直接用 `clock_gettime` |
| `detail::clock_cycles` / `read_invariant_cpu_frequency` / `invariant_cpu_freq` | **删除** | 上述分支的支撑代码，含平台汇编与 `/proc` 解析 |
| `EveryManyUS` | **删除** | 操作频率控制类，bthread 核心 0 次使用 |
| `timeval_to_*` / `*_to_timeval` 换算 | **删除** | bthread 核心 0 次使用（`gettimeofday_us` 直接内联计算） |
| `time.cpp` 的 `/proc/cpuinfo` 解析、`memmem` 依赖 | **删除** | 随 `read_cpu_frequency` 一并删除，`time.cpp` 只剩 `monotonic_time_ns` |

## 5. 与 std 的关系

C++11 的 `<chrono>` 提供了 `std::chrono::steady_clock` / `system_clock` / `high_resolution_clock`，功能上等价。保留 `butil/time` 而非替换为 `<chrono>` 的理由：

1. **零开销 + 纳秒精度**：`cpuwide_time_ns` 一次 `clock_gettime` 直接返回 `int64_t` 纳秒；`<chrono>` 的 `duration`/`time_point` 类型包装虽在 -O2 下也能优化掉，但类型系统更重、API 更冗长。
2. **与移植源码的接口一致**：`butex.cpp`、`timer_thread.cpp` 等直接调用 `gettimeofday_us()` / `timespec_to_microseconds()`，保留原名最小化移植改动，避免重写同步/定时逻辑。
3. **`timespec` 与系统调用对齐**：futex 系统调用、`clock_gettime` 原生使用 `timespec`，直接操作 `timespec` 避免了 `chrono` 与系统 API 之间的来回转换。
