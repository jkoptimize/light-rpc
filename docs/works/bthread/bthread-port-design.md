# bthread 核心移植设计方案

## Context

当前 light-rpc 项目基于 std::thread 做 RPC 流程处理，最大痛点是：

- **`MessageDispatcher::ProcessNewMessage`**：每收到一个 RPC 帧就 `std::thread(func).detach()`，高并发下创建/销毁大量 OS 线程
- **`FastChannel::CallMethod`**：调用线程阻塞在 `std::condition_variable::wait_for`，每个 in-flight RPC 占用一个 OS 线程

引入 brpc bthread（M:N 协程库）将 OS 线程与用户任务解耦：M 个 bthread 映射到 N 个 worker pthread，阻塞时自动让出 CPU。

移植目标：**保留 M:N 协程调度核心，裁剪掉 bvar 监控、gflags 配置、contention profiling 等非必要模块。命名空间 `bthread` → `fast`，`butil` → `fast::butil`。**

平台原则：**仅支持 Linux，不移植 macOS/Windows 兼容层**；这不改变现有 x86_64/aarch64 的 CPU 架构范围。优先保留原版 Linux 分支的语义和性能。`pthread_numeric_id()` 在原版 Linux 分支仅返回 `pthread_self()`，因此直接使用后者进行线程身份哈希和锁 owner 判断；不将其替换成有不同语义和调用成本的内核 TID 查询。pthread 标识不是线程控制块的可移植地址接口，也不是永久唯一 ID，在线程退出后可以复用。

源文件：`/home/syt/Desktop/brpc/brpc/src/bthread/`
目标目录：`/home/syt/rdma/light-rpc/src-bthread/`
总代码量：**~14,400 行**（bthread 核心 ~10,500 + butil 必须移植 ~5,400 - 精简裁剪 ~1,500）

---

## butil 移植决策逐文件分析

### 必须移植（复杂实现，std 无等价物）— 13 个模块

| 模块 | 行数 | 说明 | 为何不能替换 |
|------|------|------|-------------|
| `macros.h` | 496→~100 | `BAIDU_CACHELINE_ALIGNMENT`, `BAIDU_CASSERT`, `DISALLOW_COPY_AND_ASSIGN`, `ARRAY_SIZE` | 被几乎所有文件依赖，精简版只保留实际用到的宏 |
| `build_config.h` | 193→~50 | OS/编译器平台检测 | 精简为 `OS_LINUX` / `ARCH_CPU_X86_FAMILY` / `COMPILER_GCC` 三个检测 |
| `resource_pool.h` + `_inl.h` | 778 | ABA-free ID 分配器，分 Block 内嵌 T 对象，原子并发控制，version 计数防 ABA，TLS 空闲链表 | TaskMeta/TimerTask 的高频分配/释放，std 无等价物 |
| `object_pool.h` + `_inl.h` | 707 | 线程局部对象缓存池，本地 LocalPool → 全局 FreeChunk 两级结构 | ContextualStack/ButexWaiter 高频对象复用，性能关键 |
| `flat_map.h` | 640 | 开放寻址扁平哈希表，key=`void*`，连续内存布局 | butex_wait/wake 热路径上的 butex→waiter 映射，`std::unordered_map` 节点分配开销大 |
| `linked_list.h` | 201 | 侵入式双向链表，节点嵌入目标结构体 | butex waiter 队列，零额外分配，与 object_pool 配合 |
| `bounded_queue.h` | 307 | 无锁有界环形队列，原子 CAS | remote_task_queue 跨线程投递 bthread，sched 热路径 |
| `murmurhash3.h` + `.cpp` | 830 | fmix32/fmix64 高质量哈希函数 | flat_map 内部哈希 + 6 个文件直接调用，std::hash 分布质量不如 |
| `fast_rand.h` + `.cpp` | 286 | xorshift128+ 快速伪随机数 | work-stealing 每次调度需要随机选择目标，`std::mt19937` 太慢 |
| `time.h` + `.cpp` | 585 | `cpuwide_time_ns()` 读 CLOCK_MONOTONIC，`butil::Timer` | 调度计时和定时器到期判断的基础，几十处调用点 |
| `thread_local.h` + `_inl.h` | 169 | `BAIDU_THREAD_LOCAL` + `thread_atexit()` | object_pool 每线程退出时归还对象到全局池，C++ `thread_local` 析构不能表达此语义 |
| `thread_key.h` + `.cpp` | 390 | pthread_key 封装 + 析构回调 + keytable 池 | bthread TLS (`bthread_key_create/delete/setspecific/getspecific`) 底层依赖 |
| `aligned_memory.h` | 126 | `AlignedMemory<N, Align>` 对齐存储 | resource_pool + object_pool 的存储单元 |

### 不移植 — C++17 原生替换（5 个模块）

| 原模块 | 行数 | 替换方案 |
|--------|------|---------|
| `atomicops.h` + internals | 671 | `<atomic>` — `std::atomic<T>`, `std::memory_order_*` 完全等价 |
| `scoped_lock.h` + `synchronization/lock.h` | 578 | `std::mutex` + `std::lock_guard` — `BAIDU_SCOPED_LOCK(m)` 定义为 `std::lock_guard<std::mutex> _sl_(m)` |
| `unique_ptr.h` | 472 | `<memory>` — `std::unique_ptr`, C++17 下原文件退化为 `#include <memory>` |
| `type_traits.h` | 401 | `<type_traits>` — 仅 execution_queue 使用，core 不依赖 |
| `threading/platform_thread.h` | 201 | `<thread>` — `std::this_thread::yield()` + `std::this_thread::sleep_for()` |

### 不移植 — 仅非核心文件使用或可裁掉（6 个模块）

| 原模块 | 说明 |
|--------|------|
| `memory/scoped_ptr.h` | 仅 `execution_queue_inl.h` 使用，不移植 |
| `memory/scope_guard.h` | mutex.cpp 裁剪 contention profiling 后不再需要 |
| `memory/singleton_on_pthread_once.h` | `stack.cpp`/`butex.cpp` 的单例初始化，替换为 C++11 magic statics |
| `compat.h` | 仅 `task_group.cpp` 的 `OS_MACOSX` 分支，删除引用 |
| `reloadable_flags.h` | gflags 包装，已用 `FastBthreadConfig` 替代 |
| `errno.h` + `errno.cpp` | `berror()` = `strerror(errno)` 包装，内联即可 |

### 不移植 — valgrind / contention profiling 专用（7 个模块）

| 原模块 | 说明 |
|--------|------|
| `debug/address_annotations.h` | valgrind 注解，`#if 0` 掉 |
| `third_party/dynamic_annotations/` | valgrind 检测，`#if 0` 掉 |
| `third_party/valgrind/valgrind.h` | 栈注册，`#if 0` 掉 |
| `debug/stack_trace.h` | mutex contention profiling，裁掉 |
| `third_party/symbolize/symbolize.h` | mutex contention profiling，裁掉 |
| `iobuf.h` | mutex contention profiling 写文件，裁掉 |
| `fd_guard.h` + `files/*` | mutex contention profiling 文件操作，裁掉 |

---

## 移植文件清单

### bthread 核心文件

```
src-bthread/
├── context.h            # 几乎不变
├── context.cpp          # 汇编，x86_64 + aarch64 平台，不变
├── stack.h              # 小改：gflags→config, 命名空间
├── stack_inl.h          # 小改：命名空间
├── stack.cpp            # 小改：valgrind #if 0, singleton→C++11 static, gflags→config
├── task_meta.h          # 小改：bvar tracer 引用删除
├── task_group.h         # 中改：gflags→config, bvar→stripped
├── task_group_inl.h     # 小改：命名空间
├── task_group.cpp       # 中改：gflags→config, compat.h 删除, reloadable_flags 删除
├── task_control.h       # 中改：bvar 成员→普通计数器
├── task_control.cpp     # 中改：bvar 初始化删除, platform_thread→std
├── work_stealing_queue.h # 小改：logging 替换
├── remote_task_queue.h  # 小改：命名空间
├── parking_lot.h        # 小改：gflags→config
├── butex.h              # 小改：macros 引用
├── butex.cpp            # 中改：logging 替换, singleton→C++11 static
├── timer_thread.h       # 小改：命名空间
├── timer_thread.cpp     # 中改：gflags→config, platform_thread→std
├── bthread.h            # 小改：命名空间
├── bthread.cpp          # 中改：15+ 个 FLAGS_*→FastBthreadConfig, reloadable_flags 删除
├── types.h              # 小改：去掉 csite 字段
├── id.h                 # 小改：命名空间
├── id.cpp               # 小改：logging 替换
├── list_of_abafree_id.h # 小改：宏替换
├── errno.h              # 小改：命名空间, berror→strerror
├── errno.cpp            # 可删除（berror 内联替换）
├── key.cpp              # 小改：logging 替换
├── processor.h          # 不变：cpu_relax 单行宏
├── prime_offset.h       # 小改：宏替换
├── sys_futex.h          # 小改：build_config 替换
├── sys_futex.cpp        # 小改：scoped_lock→std
├── mutex.h              # 中改：去掉 bvar/collector, csite 字段
├── mutex.cpp            # **大改**：1330→~400，裁 contention profiling + pthread hook
├── condition_variable.h # 小改：命名空间
└── condition_variable.cpp # 小改：logging 替换
```

### butil 目录（只移植上述"必须移植"的 13 个模块）

```
src-bthread/butil/
├── macros.h              # 精简版 ~100 行
├── build_config.h        # 精简版 ~50 行
├── resource_pool.h
├── resource_pool_inl.h
├── object_pool.h
├── object_pool_inl.h
├── flat_map.h
├── linked_list.h
├── bounded_queue.h
├── fast_rand.h
├── fast_rand.cpp
├── time.h
├── time.cpp
├── thread_local.h
├── thread_local_inl.h
├── thread_key.h
├── thread_key.cpp
├── aligned_memory.h
└── third_party/
    └── murmurhash3/
        ├── murmurhash3.h
        └── murmurhash3.cpp
```

### 不移植的 bthread 文件

`fd.cpp`, `execution_queue.cpp/h/_inl.h`, `task_tracer.cpp/h`, `semaphore.cpp`, `rwlock.cpp/h`, `countdown_event.cpp/h`, `interrupt_pthread.cpp/h`, `comlog_initializer.h`, `singleton_on_bthread_once.h`

### 新增文件

```
inc/fast_bthread_config.h   # FastBthreadConfig 结构体，替代 gflags
```

---

## 关键修改点

### 1. 配置系统：gflags → `FastBthreadConfig`

```cpp
// inc/fast_bthread_config.h

namespace fast {

struct FastBthreadConfig {
    int bthread_concurrency      = 8 + 1;  // FLAGS_bthread_concurrency (+1 for epoll)
    int bthread_min_concurrency  = 0;      // FLAGS_bthread_min_concurrency
    int task_group_runqueue_capacity = 4096; // FLAGS_task_group_runqueue_capacity
    int stack_size_small   = 32768;       // FLAGS_stack_size_small
    int stack_size_normal  = 1048576;     // FLAGS_stack_size_normal
    int stack_size_large   = 8388608;     // FLAGS_stack_size_large
    int guard_page_size    = 4096;        // FLAGS_guard_page_size
    int tc_stack_small     = 32;          // FLAGS_tc_stack_small
    int tc_stack_normal    = 8;           // FLAGS_tc_stack_normal
    int task_group_ntags          = 1;    // FLAGS_task_group_ntags
    int parking_lot_of_each_tag   = 4;    // FLAGS_bthread_parking_lot_of_each_tag
    int timer_granularity_us      = 1000; // Unsched sleep granularity

    static FastBthreadConfig& Get() {
        static FastBthreadConfig config;
        return config;
    }
};

} // namespace fast
```

所有 `FLAGS_xxx` → `FastBthreadConfig::Get().xxx`。

### 2. C++17 原生替换汇总

| 原 butil | 替换为 | 改动点 |
|----------|--------|--------|
| `butil::atomic<T>` | `std::atomic<T>` | 全局查找替换，包含 `<atomic>` |
| `butil::memory_order_*` | `std::memory_order_*` | 全局查找替换 |
| `butil::Mutex` | `std::mutex` | `task_control.h` 中 ~3 处成员变量 |
| `BAIDU_SCOPED_LOCK(m)` | `std::lock_guard<std::mutex> _sl_(m)` | 宏定义在精简 `macros.h` 中 |
| `butil::unique_ptr<T>` | `std::unique_ptr<T>` | include 替换为 `<memory>` |
| `YieldThread()` | `std::this_thread::yield()` | `task_control.cpp`, `timer_thread.cpp` |
| `Sleep(us)` | `std::this_thread::sleep_for(microseconds(us))` | `task_control.cpp`, `timer_thread.cpp` |
| `singleton_on_pthread_once` | C++11 magic static | `stack.cpp`, `butex.cpp` |
| `berror()` | `strerror(errno)` | 删除 `butil/errno.h` 依赖 |
| `#include "butil/reloadable_flags.h"` | `#include "inc/fast_bthread_config.h"` | `task_group.cpp`, `bthread.cpp` |
| `#include "butil/logging.h"` | 项目 CHECK/LOG 宏 | 所有文件 |
| `#include "butil/unique_ptr.h"` | `#include <memory>` | `task_group.cpp`, `mutex.cpp` |
| `#include "butil/compat.h"` | 删除 | `task_group.cpp` |

### 3. bvar 剥离

`task_control.h` 中 ~20 个 bvar 成员全部替换为普通计数器：

```cpp
// 原来：
bvar::Adder<int64_t> _nworkers;
bvar::PassiveStatus<double> _cumulated_worker_time;
bvar::PerSecond<bvar::PassiveStatus<double>> _worker_usage_second;

// 替换为：
int64_t _nworkers{0};
double _cumulated_worker_time{0.0};
// _worker_usage_second 直接删除
```

`task_control.cpp` 中对应初始化代码（如 `_nworkers << "bthread_worker_count"`）删除。

### 4. mutex.cpp 裁剪

加 `#ifndef FAST_MUTEX_NO_CONTENTION`，默认在 CMake 定义，裁掉 ~900 行：

- contention profiler (`g_cp`, `ContentionProfiler`, `SampledContention`)
- pthread_mutex interposition / `dlsym` hook
- `BRPC_DEBUG_LOCK` 死锁检测
- `BTHREAD_USE_FAST_PTHREAD_MUTEX` 平台选择宏：当前仅支持 Linux，固定移植其启用分支的原子状态 + futex 实现，不再使用 `std::mutex` 替代 `internal::FastPthreadMutex`（2026-10-06 修正，见 [mutex 分层与移植说明](../../knowledge/brpc-mutex-hooks.md)）。
- 移除对 `iobuf.h`, `fd_guard.h`, `files/*.h`, `stack_trace.h`, `symbolize.h`, `murmurhash3`, `object_pool` 的依赖

### 5. Valgrind 守卫

`stack.cpp` 中 valgrind 相关调用用 `#if 0` 包裹。`object_pool_inl.h` 中 `address_annotations.h` 同样处理。

---

## 构建集成

`src-bthread/CMakeLists.txt`：

```cmake
set(BTHREAD_SOURCES
    context.cpp stack.cpp task_group.cpp task_control.cpp
    butex.cpp timer_thread.cpp bthread.cpp id.cpp
    key.cpp sys_futex.cpp mutex.cpp condition_variable.cpp
    butil/fast_rand.cpp butil/time.cpp butil/thread_key.cpp
    butil/third_party/murmurhash3/murmurhash3.cpp
)

add_library(fast_bthread STATIC ${BTHREAD_SOURCES})
target_include_directories(fast_bthread PUBLIC
    ${PROJECT_SOURCE_DIR}/src-bthread
    ${PROJECT_SOURCE_DIR}           # for inc/fast_bthread_config.h
)
target_link_libraries(fast_bthread PUBLIC pthread)
target_compile_definitions(fast_bthread PRIVATE FAST_MUTEX_NO_CONTENTION)
```

依赖：仅 `pthread`（`pthread_mutex`, `pthread_spinlock`, `pthread_key_create`, `futex` syscall）。不需要 gflags、protobuf、unwind、absl、glog。

---

## 实施步骤

### Phase 1: 创建目录 + butil 模块

1. 创建 `src-bthread/butil/third_party/murmurhash3/`
2. 复制 13 个必须移植的 butil 模块（头 + cpp）
3. 精简 `macros.h` 和 `build_config.h`（只保留实际用到的宏）
4. 命名空间替换：`butil` → `fast::butil`；`BAIDU_*` 宏加别名
5. 替换 `butil/atomicops.h` 引用为 `<atomic>`，替换 `butil::atomic` 为 `std::atomic`
6. 替换 `scoped_lock.h` 中的 `butil::Mutex` 为 `std::mutex`
7. `object_pool_inl.h` 中 `address_annotations.h` → `#if 0`
8. 添加 `src-bthread/butil/` 下的 `CMakeLists.txt`（或在上层统一配置）

### Phase 2: 复制 + 修改 bthread 核心文件

1. 复制 34 个 bthread 核心文件到 `src-bthread/`
2. 全局替换：
   - `#include "bthread/` → `#include "`（平铺目录，无子目录前缀）
   - `#include "butil/` → `#include "butil/`（路径不变）
   - `namespace bthread` → `namespace fast`
   - `FLAGS_*` → `FastBthreadConfig::Get().*`
   - `LOG(*)/CHECK(*)` → 项目宏
   - `butil::atomic` → `std::atomic`
   - `butil::Mutex` → `std::mutex`
   - `BAIDU_SCOPED_LOCK` → `std::lock_guard`
   - `butil::unique_ptr` → `std::unique_ptr`
   - `YieldThread()` → `std::this_thread::yield()`
   - `Sleep(us)` → `std::this_thread::sleep_for()`
3. 删除：`#include "butil/logging.h"`, `"butil/reloadable_flags.h"`, `"butil/compat.h"`, `"butil/unique_ptr.h"`
4. 删除：`#include "bvar/` 及所有 bvar 使用
5. `stack.cpp` 中 valgrind 调用 `#if 0`，`singleton` → C++11 magic static
6. `butex.cpp` 中 `singleton` → C++11 magic static
7. 添加 `#include "inc/fast_bthread_config.h"`

### Phase 3: mutex.cpp 裁剪

1. 添加 `#ifndef FAST_MUTEX_NO_CONTENTION` 守卫 contention profiling 和 pthread hook 代码块
2. 移除对 `iobuf.h`, `fd_guard.h`, `files/*.h`, `stack_trace.h`, `symbolize.h`, `scope_guard.h`, `murmurhash3.h`, `object_pool.h`, `thread_local.h` 的引用
3. 从 ~1330 行缩减到 ~400 行

### Phase 4: CMakeLists.txt + 编译验证

1. 编写 `src-bthread/CMakeLists.txt`，在主 `CMakeLists.txt` 中添加 `add_subdirectory(src-bthread)`
2. `cd build && cmake .. && make -j$(nproc)`
3. 修编译错误

### Phase 5: 基础功能验证

1. 单元测试：创建 bthread → 执行简单函数 → join
2. butex_wait / butex_wake 正确同步
3. bthread_mutex lock/unlock 正确互斥
4. bthread_cond wait/signal
5. bthread_usleep 计时精度
6. 多 worker work-stealing 验证
7. valgrind 无内存泄漏
8. mutex.cpp 裁剪后功能正常

---

## 验证方法

```bash
cd build && cmake .. && make -j$(nproc)
./bthread_test

# 验证要点：
# 1. bthread_start 创建返回 bthread_t，bthread_join 等待完成
# 2. bthread_mutex_lock/unlock 互斥正确
# 3. butex_wait/butex_wake 同步正确
# 4. bthread_usleep 计时精度（误差 < 10%）
# 5. 无内存泄漏（valgrind 验证）
```

---

## 风险

1. **栈溢出**：bthread 使用固定大小栈（32KB/1MB/8MB），默认 1MB。用户函数栈超限会不报错 crash。调试用 LARGE 栈。
2. **futex 限制**：依赖 Linux `SYS_futex`，仅支持 Linux（WSL2 可正常工作）。
3. **TLS 冲突**：bthread 内部用 `BAIDU_THREAD_LOCAL` 保存当前 TaskGroup，注意与项目现有 TLS 不冲突。
4. **resource_pool / object_pool 线程安全**：原来用 `butil::Mutex`（pthread_mutex 包装），改为 `std::mutex` 无实质差异。
