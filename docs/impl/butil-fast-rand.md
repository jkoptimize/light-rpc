# butil/fast_rand.h + fast_rand.cpp — 快速随机数生成

## 1. 作用概述

`fast_rand` 提供**无全局竞争、纳秒级开销**的伪随机数生成，核心是 **xorshift128+** 算法。

关键设计是 **TLS（线程局部）种子**：每个线程持有自己的 `_tls_seed`（`static __thread`），调用 `fast_rand()` 时只读写本线程的种子，**完全不碰任何共享状态、不加锁**。这避免了 `rand()` 的全局锁竞争和 `std::mt19937` 的笨重状态，把单次生成成本压到约 5ns。

种子初始化用 `splitmix64`（一种通过 BigCrush 测试的快速生成器）+ `gettimeofday_us()` 作为熵源；`fast_rand_less_than(range)` 通过「拒绝采样 + 区间划分」产生均匀分布在 `[0, range)` 的无偏整数。

## 2. 关键 API

| 符号 | 签名 | 说明 |
|------|------|------|
| `FastRandSeed` | `struct { uint64_t s[2]; }` | xorshift128+ 的 128 位状态 |
| `fast_rand()` | `uint64_t` | 从 TLS 种子生成 64 位随机数（~5ns，无锁） |
| `fast_rand(FastRandSeed*)` | `uint64_t` | 从给定种子生成（供需要显式状态者使用） |
| `fast_rand_less_than(range)` | `uint64_t` | 生成 `[0, range)` 均匀分布整数（~30ns） |
| `init_fast_rand_seed(seed)` | `void` | 用 `gettimeofday_us` + splitmix64 初始化种子 |

内部实现函数（`inline`，不出头文件）：
- `xorshift128_next(seed)`：xorshift128+ 核心，一次迭代产生一个 64 位随机数。
- `splitmix64_next(seed)`：种子播种器。
- `fast_rand_impl(range, seed)`：无偏区间映射（拒绝采样）。

## 3. 在 bthread 中的作用

随机数是 bthread **work-stealing（工作窃取）调度**的必需组件。bthread 的调度模型是：每个 worker 线程有一个本地 runqueue，当 worker 本地队列空了，它会去**随机挑选一个其他 worker** 偷取任务。随机选择的均匀性直接影响负载均衡，而选择动作发生在**每次调度**的热路径上。

具体使用点：

1. **`task_group.h` — `_steal_seed`**：每个 `TaskGroup` 持有 `size_t _steal_seed{fast_rand()}`，作为该 group 窃取目标随机化的种子。
2. **`task_control.cpp` — `groups[fast_rand_less_than(ngroup)]`**：创建新 bthread 时，从多个 `TaskGroup` 中**随机挑一个**投递任务，把新建任务均匀分散到各 worker 的队列，避免单队列热点。
3. **`prime_offset.h` — `prime_offset(fast_rand())`**：用随机数计算哈希表（`flat_map`）的探测偏移，避免固定步长导致的聚集。

因为这些调用点在调度热路径上，**随机数生成的开销直接计入每次上下文切换/任务投递的延迟**，所以必须用 xorshift128+ 这种无锁、纳秒级、且统计质量足够（通过 BigCrush）的实现，而不能用 `std::mt19937`（状态重、分配/初始化昂贵）。

## 4. 移植裁剪决策

| 原文件内容 | 决策 | 理由 |
|------------|------|------|
| `fast_rand_in<T>(min, max)` 模板 | **删除** | bthread 核心 0 次使用 |
| `fast_rand_in_64` / `fast_rand_in_u64` | **删除** | 仅被 `fast_rand_in` 模板调用，随之删除 |
| `fast_rand_double()` | **删除** | 0 次使用；依赖 `<math.h>`（`ldexp`）和 `COMPILE_ASSERT` |
| `fast_rand_bytes()` | **删除** | 0 次使用 |
| `fast_rand_printable()` | **删除** | 0 次使用；依赖 `<string>` |
| `#include "butil/numerics/safe_conversions.h"`（`safe_abs`） | **删除** | 仅 `fast_rand_in_64` 使用，随其删除 |
| `#include "butil/basictypes.h"` / `"butil/macros.h"` | **删除** | 裁剪后不再需要（`BAIDU_UNLIKELY`、`COMPILE_ASSERT` 的用途都已删） |
| `butil::gettimeofday_us()` | **改为非限定 `gettimeofday_us()`** | 已在 `fast::butil` 命名空间内，直接调用 |

裁剪后 `fast_rand.cpp` 仅依赖 `time.h`（`gettimeofday_us`）和 `<limits>`（`numeric_limits`），无 `macros.h` 依赖。

## 5. 与 std 的关系

C++ 标准库提供 `<random>`（`std::mt19937`、`std::uniform_int_distribution` 等），但保留 `fast_rand` 的理由：

1. **性能**：`std::mt19937` 状态为 2500 字节，每次构造/拷贝代价高，`uniform_int_distribution` 有额外开销；xorshift128+ 状态仅 16 字节，一次迭代约 5ns。在 work-stealing 热路径上差距显著。
2. **无全局竞争**：`fast_rand` 用 `__thread` TLS 种子，天然线程安全且无锁；`std::random_device` 有系统调用开销，`std::mt19937` 需每线程显式维护 `thread_local` 对象。
3. **与移植源码一致**：`task_group.h`、`task_control.cpp`、`prime_offset.h` 直接调用 `fast_rand()` / `fast_rand_less_than()`，保留原名最小化改动。
