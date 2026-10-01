# bthread 核心移植实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 brpc bthread M:N 协程库核心移植到 light-rpc 项目，替换当前 std::thread 依赖

**Architecture:** 从 `/home/syt/Desktop/brpc/brpc/src/bthread/` 复制核心文件到 `src-bthread/`，命名空间 `bthread`→`fast`，`butil`→`fast::butil`。gflags→FastBthreadConfig，C++17 std 替换 butil 同步原语，裁掉 bvar/valgrind/contention profiling。只依赖 pthread。

**Tech Stack:** C++17, CMake, pthread, Linux futex, x86_64+aarch64 汇编

## Global Constraints

- 平台范围：仅支持 Linux，不为 macOS/Windows 移植兼容层；Linux 专用接口可直接使用。CPU 架构仍按现有 x86_64/aarch64 支持范围处理。
- 保持原版 Linux 分支的线程身份语义：`pthread_numeric_id()` 直接替换为 `pthread_self()`，用于线程身份哈希和锁 owner 判断。它是 pthread 标识，不是内核 TID；只有明确需要内核 TID 的场景才使用 `gettid`/`SYS_gettid`，不在热点路径无故增加系统调用。pthread 标识在线程退出后可能复用，不能作为永久唯一 ID。
- 命名空间：`bthread` → `fast`，`butil` → `fast::butil`
- gflags 全部替换为 `FastBthreadConfig::Get().xxx`
- `butil::atomic` → `std::atomic`，`butil::Mutex` → `std::mutex`
- `BAIDU_SCOPED_LOCK` → `std::lock_guard`
- `butil::unique_ptr` → `std::unique_ptr`
- `YieldThread()` → `std::this_thread::yield()`
- `Sleep(us)` → `std::this_thread::sleep_for()`
- singleton_on_pthread_once → C++11 magic static
- `#include "bthread/` → `#include "`（平铺目录）
- bvar 成员按用途审查；无消费者的 `_nworkers`、`_nbthreads` 直接裁剪，不替换为共享计数器；调度控制状态保留。
- valgrind 调用 → `#if 0`
- mutex.cpp 加 `FAST_MUTEX_NO_CONTENTION` 裁掉 ~900 行
- 编译验证每步通过后方可进入下一步

---

### Task 1: 创建目录结构

**Files:**
- Create: `src-bthread/`
- Create: `src-bthread/butil/`
- Create: `src-bthread/butil/third_party/murmurhash3/`
- Create: `inc/fast_bthread_config.h`

**Interfaces:**
- Produces: 空目录结构，后续任务填充文件

- [ ] **Step 1: 创建目录**

```bash
mkdir -p /home/syt/rdma/light-rpc/src-bthread/butil/third_party/murmurhash3
```

- [ ] **Step 2: 创建 FastBthreadConfig 头文件**

Write `inc/fast_bthread_config.h`：

```cpp
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
```

- [ ] **Step 3: 验证**

```bash
ls /home/syt/rdma/light-rpc/src-bthread/butil/third_party/murmurhash3
ls /home/syt/rdma/light-rpc/inc/fast_bthread_config.h
```

- [ ] **Step 4: Commit**

```bash
git add src-bthread/ inc/fast_bthread_config.h
git commit -m "chore: create src-bthread directory structure and FastBthreadConfig"
```

---

### Task 2: 移植 butil 基础模块（macros, build_config, aligned_memory, fast_rand, time）

**Files:**
- Create: `src-bthread/butil/macros.h`
- Create: `src-bthread/butil/build_config.h`
- Create: `src-bthread/butil/aligned_memory.h`
- Create: `src-bthread/butil/fast_rand.h`
- Create: `src-bthread/butil/fast_rand.cpp`
- Create: `src-bthread/butil/time.h`
- Create: `src-bthread/butil/time.cpp`

**Interfaces:**
- Produces: `BAIDU_CACHELINE_ALIGNMENT`, `BAIDU_CASSERT`, `DISALLOW_COPY_AND_ASSIGN`, `ARRAY_SIZE`, `OS_LINUX`, `ARCH_CPU_X86_FAMILY`, `COMPILER_GCC`, `AlignedMemory<N,Align>`, `fast_rand()`, `cpuwide_time_ns()`, `butil::Timer`

- [ ] **Step 1: 复制 macros.h 并精简**

从 `/home/syt/Desktop/brpc/brpc/src/butil/macros.h` 复制，只保留以下实际用到的宏，放入 `src-bthread/butil/macros.h`：

```cpp
#ifndef FAST_BUTIL_MACROS_H
#define FAST_BUTIL_MACROS_H

#include <cstddef>

#ifndef BAIDU_CACHELINE_ALIGNMENT
#define BAIDU_CACHELINE_ALIGNMENT __attribute__((aligned(64)))
#endif

#define BAIDU_CASSERT(expr, msg) static_assert(expr, #msg)

#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
    TypeName(const TypeName&) = delete;    \
    void operator=(const TypeName&) = delete

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#define ALLOW_UNUSED __attribute__((unused))

#ifdef NDEBUG
#define BAIDU_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define BAIDU_UNLIKELY(x) (x)
#endif

#endif // FAST_BUTIL_MACROS_H
```

- [ ] **Step 2: 复制 build_config.h 并精简**

从 `/home/syt/Desktop/brpc/brpc/src/butil/build_config.h` 复制，保留平台检测宏，放入 `src-bthread/butil/build_config.h`：

```cpp
#ifndef FAST_BUTIL_BUILD_CONFIG_H
#define FAST_BUTIL_BUILD_CONFIG_H

#if defined(__linux__)
#define OS_LINUX 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_CPU_X86_FAMILY 1
#define ARCH_CPU_64_BITS 1
#elif defined(__aarch64__)
#define ARCH_CPU_ARM_FAMILY 1
#define ARCH_CPU_64_BITS 1
#endif

#if defined(__GNUC__)
#define COMPILER_GCC 1
#endif

#endif // FAST_BUTIL_BUILD_CONFIG_H
```

- [ ] **Step 3: 复制 aligned_memory.h**

从 `/home/syt/Desktop/brpc/brpc/src/butil/memory/aligned_memory.h` 直接复制到 `src-bthread/butil/aligned_memory.h`。命名空间 `butil` 保持不变（后续全局替换）。

```bash
cp /home/syt/Desktop/brpc/brpc/src/butil/memory/aligned_memory.h \
   /home/syt/rdma/light-rpc/src-bthread/butil/aligned_memory.h
```

- [ ] **Step 4: 复制 fast_rand.h 和 fast_rand.cpp**

```bash
cp /home/syt/Desktop/brpc/brpc/src/butil/fast_rand.h \
   /home/syt/rdma/light-rpc/src-bthread/butil/fast_rand.h
cp /home/syt/Desktop/brpc/brpc/src/butil/fast_rand.cpp \
   /home/syt/rdma/light-rpc/src-bthread/butil/fast_rand.cpp
```

修改 fast_rand.h 的 include 路径：`#include "butil/macros.h"` → `#include "macros.h"`
修改 fast_rand.cpp 的 include 路径：`#include "butil/fast_rand.h"` → `#include "fast_rand.h"`

- [ ] **Step 5: 复制 time.h 和 time.cpp**

```bash
cp /home/syt/Desktop/brpc/brpc/src/butil/time.h \
   /home/syt/rdma/light-rpc/src-bthread/butil/time.h
cp /home/syt/Desktop/brpc/brpc/src/butil/time.cpp \
   /home/syt/rdma/light-rpc/src-bthread/butil/time.cpp
```

time.h 中 包含 `"butil/build_config.h"` → `"build_config.h"`，`"butil/compat.h"` → 删除该 include。
time.cpp 中 `#include "butil/time.h"` → `#include "time.h"`，`#include "butil/scoped_lock.h"` → `#include <mutex>`。

- [ ] **Step 6: 验证**

```bash
ls /home/syt/rdma/light-rpc/src-bthread/butil/macros.h
ls /home/syt/rdma/light-rpc/src-bthread/butil/build_config.h
ls /home/syt/rdma/light-rpc/src-bthread/butil/aligned_memory.h
ls /home/syt/rdma/light-rpc/src-bthread/butil/fast_rand.h
ls /home/syt/rdma/light-rpc/src-bthread/butil/fast_rand.cpp
ls /home/syt/rdma/light-rpc/src-bthread/butil/time.h
ls /home/syt/rdma/light-rpc/src-bthread/butil/time.cpp
```

- [ ] **Step 7: Commit**

```bash
git add src-bthread/butil/
git commit -m "chore: port butil base modules (macros, build_config, aligned_memory, fast_rand, time)"
```

---

### Task 3: 移植 butil 线程模块（thread_local, thread_key）

**Files:**
- Create: `src-bthread/butil/thread_local.h`
- Create: `src-bthread/butil/thread_local_inl.h`
- Create: `src-bthread/butil/thread_key.h`
- Create: `src-bthread/butil/thread_key.cpp`

**Interfaces:**
- Produces: `BAIDU_THREAD_LOCAL`, `thread_atexit()`, `SingleThreaded/SetThreadLocal/GetThreadLocal`

- [ ] **Step 1: 复制 thread_local.h 和 thread_local_inl.h**

```bash
cp /home/syt/Desktop/brpc/brpc/src/butil/thread_local.h \
   /home/syt/rdma/light-rpc/src-bthread/butil/thread_local.h
cp /home/syt/Desktop/brpc/brpc/src/butil/thread_local_inl.h \
   /home/syt/rdma/light-rpc/src-bthread/butil/thread_local_inl.h
```

修改 include：
- `thread_local.h` 中 `#include "butil/build_config.h"` → `#include "build_config.h"`
- `thread_local_inl.h` 中 `#include "butil/thread_local.h"` → `#include "thread_local.h"`

- [ ] **Step 2: 复制 thread_key.h 和 thread_key.cpp**

```bash
cp /home/syt/Desktop/brpc/brpc/src/butil/thread_key.h \
   /home/syt/rdma/light-rpc/src-bthread/butil/thread_key.h
cp /home/syt/Desktop/brpc/brpc/src/butil/thread_key.cpp \
   /home/syt/rdma/light-rpc/src-bthread/butil/thread_key.cpp
```

修改 include：
- `thread_key.h` 中 `#include "butil/build_config.h"` → `#include "build_config.h"`
- `thread_key.cpp` 中 `#include "butil/thread_key.h"` → `#include "thread_key.h"`

- [ ] **Step 3: 验证**

```bash
wc -l src-bthread/butil/thread_local.h src-bthread/butil/thread_key.h
```

- [ ] **Step 4: Commit**

```bash
git add src-bthread/butil/thread_local.h src-bthread/butil/thread_local_inl.h \
        src-bthread/butil/thread_key.h src-bthread/butil/thread_key.cpp
git commit -m "chore: port butil thread modules (thread_local, thread_key)"
```

---

### Task 4: 移植 butil 容器和哈希模块（flat_map, linked_list, bounded_queue, murmurhash3, resource_pool, object_pool）

**Files:**
- Create: `src-bthread/butil/flat_map.h`
- Create: `src-bthread/butil/linked_list.h`
- Create: `src-bthread/butil/bounded_queue.h`
- Create: `src-bthread/butil/resource_pool.h`
- Create: `src-bthread/butil/resource_pool_inl.h`
- Create: `src-bthread/butil/object_pool.h`
- Create: `src-bthread/butil/object_pool_inl.h`
- Create: `src-bthread/butil/third_party/murmurhash3/murmurhash3.h`
- Create: `src-bthread/butil/third_party/murmurhash3/murmurhash3.cpp`

**Interfaces:**
- Produces: `FlatMap<K,V>`, `LinkNode<T>`, `BoundedQueue<T>`, `fmix64()/fmix32()`, `ResourceId<T>`, `get_resource()/return_resource()`, `get_object<T>()/return_object()`

- [ ] **Step 1: 复制所有容器和池文件**

```bash
SRC=/home/syt/Desktop/brpc/brpc/src/butil
DST=/home/syt/rdma/light-rpc/src-bthread/butil

cp $SRC/containers/flat_map.h        $DST/flat_map.h
cp $SRC/containers/linked_list.h     $DST/linked_list.h
cp $SRC/containers/bounded_queue.h   $DST/bounded_queue.h
cp $SRC/resource_pool.h              $DST/resource_pool.h
cp $SRC/resource_pool_inl.h          $DST/resource_pool_inl.h
cp $SRC/object_pool.h                $DST/object_pool.h
cp $SRC/object_pool_inl.h            $DST/object_pool_inl.h
cp $SRC/third_party/murmurhash3/murmurhash3.h   $DST/third_party/murmurhash3/murmurhash3.h
cp $SRC/third_party/murmurhash3/murmurhash3.cpp $DST/third_party/murmurhash3/murmurhash3.cpp
```

- [ ] **Step 2: 修改所有文件的 include 路径**

每个文件中的 `#include "butil/xxx.h"` 改为相对路径引用：

- `flat_map.h`：`#include "butil/macros.h"` → `#include "macros.h"`；`#include "butil/third_party/murmurhash3/murmurhash3.h"` → `#include "third_party/murmurhash3/murmurhash3.h"`
- `linked_list.h`：`#include "butil/macros.h"` → `#include "macros.h"`
- `bounded_queue.h`：`#include "butil/macros.h"` → `#include "macros.h"`；`#include "butil/atomicops.h"` → `#include <atomic>`
- `resource_pool.h`：`#include "butil/macros.h"` → `#include "macros.h"`
- `resource_pool_inl.h`：所有 `#include "butil/xxx"` → 相对路径：
  - `"butil/atomicops.h"` → `<atomic>`
  - `"butil/macros.h"` → `"macros.h"`
  - `"butil/scoped_lock.h"` → ——删除，替换为 `<mutex>`——（见 Step 5）
  - `"butil/thread_local.h"` → `"thread_local.h"`
  - `"butil/memory/aligned_memory.h"` → `"aligned_memory.h"`
- `object_pool.h`：无需修改
- `object_pool_inl.h`：同上模式：
  - `"butil/atomicops.h"` → `<atomic>`
  - `"butil/macros.h"` → `"macros.h"`
  - `"butil/scoped_lock.h"` → ——删除，替换为 `<mutex>`——（见 Step 5）
  - `"butil/thread_local.h"` → `"thread_local.h"`
  - `"butil/memory/aligned_memory.h"` → `"aligned_memory.h"`
  - `"butil/debug/address_annotations.h"` → 删除（`#if 0`）
- `murmurhash3.cpp`：`#include "butil/third_party/murmurhash3/murmurhash3.h"` → `#include "third_party/murmurhash3/murmurhash3.h"`

- [ ] **Step 3: 修复 atomicops 和 scoped_lock 引用**

在 `bounded_queue.h`, `resource_pool_inl.h`, `object_pool_inl.h` 中：
- 删除 `#include "butil/atomicops.h"`，添加 `#include <atomic>`
- 将 `butil::atomic<T>` 替换为 `std::atomic<T>`

- [ ] **Step 4: 处理 valgrind 依赖**

在 `object_pool_inl.h` 中，删除 `#include "butil/debug/address_annotations.h"`，将其用途用 `#if 0` 包裹：

```cpp
// Valgrind annotations disabled for port
#if 0
// original valgrind code
#endif
```

- [ ] **Step 5: 替换 BAIDU_SCOPED_LOCK 为 std::lock_guard**

在 `resource_pool_inl.h` 和 `object_pool_inl.h` 中：
- `BAIDU_SCOPED_LOCK(lock)` → `std::lock_guard<std::mutex> _sl_(lock)`
- `butil::Mutex` 成员改为 `std::mutex`（检查是否有实际的 Mutex 成员变量声明）

同时将这两个文件中的 `#include <pthread.h>` 保留（pthread mutex 不需要了，但可能其他地方用到 pthread）。

- [ ] **Step 6: 验证文件完整性**

```bash
wc -l src-bthread/butil/*.h src-bthread/butil/*.cpp src-bthread/butil/third_party/murmurhash3/*
```

- [ ] **Step 7: Commit**

```bash
git add src-bthread/butil/
git commit -m "chore: port butil containers, pools, and murmurhash3"
```

---

### Task 5: 复制 bthread 核心文件（无修改，纯复制）

**Files:**
- Create: 34 个 bthread 文件

**Interfaces:**
- Produces: 所有 `.cpp/.h` 的原始副本，后续任务进行修改

- [ ] **Step 1: 批量复制 bthread 核心文件**

```bash
SRC=/home/syt/Desktop/brpc/brpc/src/bthread
DST=/home/syt/rdma/light-rpc/src-bthread

# 核心 .h 文件
cp $SRC/context.h              $DST/context.h
cp $SRC/stack.h                $DST/stack.h
cp $SRC/stack_inl.h            $DST/stack_inl.h
cp $SRC/task_meta.h            $DST/task_meta.h
cp $SRC/task_group.h           $DST/task_group.h
cp $SRC/task_group_inl.h       $DST/task_group_inl.h
cp $SRC/task_control.h         $DST/task_control.h
cp $SRC/work_stealing_queue.h  $DST/work_stealing_queue.h
cp $SRC/remote_task_queue.h    $DST/remote_task_queue.h
cp $SRC/parking_lot.h          $DST/parking_lot.h
cp $SRC/butex.h                $DST/butex.h
cp $SRC/timer_thread.h         $DST/timer_thread.h
cp $SRC/bthread.h              $DST/bthread.h
cp $SRC/types.h                $DST/types.h
cp $SRC/id.h                   $DST/id.h
cp $SRC/list_of_abafree_id.h   $DST/list_of_abafree_id.h
cp $SRC/errno.h                $DST/errno.h
cp $SRC/processor.h            $DST/processor.h
cp $SRC/prime_offset.h         $DST/prime_offset.h
cp $SRC/sys_futex.h            $DST/sys_futex.h
cp $SRC/mutex.h                $DST/mutex.h
cp $SRC/condition_variable.h   $DST/condition_variable.h

# 核心 .cpp 文件
cp $SRC/context.cpp            $DST/context.cpp
cp $SRC/stack.cpp              $DST/stack.cpp
cp $SRC/task_group.cpp         $DST/task_group.cpp
cp $SRC/task_control.cpp       $DST/task_control.cpp
cp $SRC/butex.cpp              $DST/butex.cpp
cp $SRC/timer_thread.cpp       $DST/timer_thread.cpp
cp $SRC/bthread.cpp            $DST/bthread.cpp
cp $SRC/id.cpp                 $DST/id.cpp
cp $SRC/errno.cpp              $DST/errno.cpp
cp $SRC/key.cpp                $DST/key.cpp
cp $SRC/sys_futex.cpp          $DST/sys_futex.cpp
cp $SRC/mutex.cpp              $DST/mutex.cpp
cp $SRC/condition_variable.cpp $DST/condition_variable.cpp

# 不复制：fd.cpp, execution_queue*, task_tracer*, semaphore*, rwlock*,
#          countdown_event*, interrupt_pthread*, comlog_initializer.h,
#          singleton_on_bthread_once.h
```

- [ ] **Step 2: 验证文件数量**

```bash
ls src-bthread/*.h src-bthread/*.cpp | wc -l
# 预期：35 个文件
```

- [ ] **Step 3: Commit**

```bash
git add src-bthread/*.h src-bthread/*.cpp
git commit -m "chore: copy bthread core source files (unmodified)"
```

---

### Task 6: 全局机械替换 — 命名空间和 include 路径

**Files:**
- Modify: `src-bthread/*.h`（所有头文件）
- Modify: `src-bthread/*.cpp`（所有源文件）

**Interfaces:**
- Consumes: 原始 bthread 文件
- Produces: 命名空间和 include 路径已修正的文件

- [ ] **Step 1: 替换 include 路径**

在 `src-bthread/` 下所有 `.h` 和 `.cpp` 文件中执行：

```bash
cd /home/syt/rdma/light-rpc/src-bthread

# bthread/ → 本地引用（文件在同一目录下）
find . -maxdepth 1 -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's|#include "bthread/\([^"]*\)"|#include "\1"|g'

# butil/ → butil/（相对路径不变，但确保前缀正确）
# 这个不需要改，butil 在子目录中
```

- [ ] **Step 2: 替换命名空间**

```bash
cd /home/syt/rdma/light-rpc/src-bthread

# bthread namespace → fast
find . -maxdepth 1 -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/namespace bthread/namespace fast/g'
find . -maxdepth 1 -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/}  \/\/ namespace bthread/}  \/\/ namespace fast/g'
find . -maxdepth 1 -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's|} // namespace bthread|} // namespace fast|g'
find . -maxdepth 1 -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/}  \/\/ namespace bthread/}  \/\/ namespace fast/g'

# butil namespace → fast::butil
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/namespace butil/namespace fast::butil/g'
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/using namespace butil/using namespace fast::butil/g'

# 开放 using 语句中的 bthread::
find . -maxdepth 1 -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/bthread::/fast::/g'
```

- [ ] **Step 3: 替换 include 中的 logging、reloadable_flags、compat 等移除的依赖**

```bash
cd /home/syt/rdma/light-rpc/src-bthread

# 删除不移植的 butil 头文件引用
find . -maxdepth 1 -name '*.cpp' | xargs sed -i \
    '/#include "butil\/logging.h"/d'
find . -maxdepth 1 -name '*.cpp' | xargs sed -i \
    '/#include "butil\/compat.h"/d'
find . -maxdepth 1 -name '*.cpp' | xargs sed -i \
    '/#include "butil\/reloadable_flags.h"/d'
find . -maxdepth 1 -name '*.cpp' | xargs sed -i \
    '/#include "butil\/unique_ptr.h"/d'

# 替换 errno.h 中的 butil/errno.h 引用
find . -maxdepth 1 -name 'errno.h' | xargs sed -i \
    's|#include "butil/errno.h"|#include <cstring> // for strerror|g'
```

- [ ] **Step 4: 替换 butil 头文件中的命名空间**

```bash
cd /home/syt/rdma/light-rpc/src-bthread/butil

# namespace butil → namespace fast::butil
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/namespace butil/namespace fast { namespace butil/g'
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/}  \/\/ namespace butil/}}  \/\/ namespace fast::butil/g'
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's|} // namespace butil|}} // namespace fast::butil|g'
```

对于 butil 内部引用 `butil::` 的代码，因为我们在 `fast::butil` 命名空间内，需要保持 `butil::` 为未限定引用。这部分较复杂，手工检查每个文件中的 `butil::` 前缀，改为在正确的命名空间内直接引用。

- [ ] **Step 5: 验证**

```bash
# 确认没有残留的 bthread:: 引用（除了注释和字符串）
grep -r "bthread::" src-bthread/*.h src-bthread/*.cpp | grep -v "// " | grep -v "//"
# 预期：少量合法残留（如字符串中的引用），确认后处理
```

- [ ] **Step 6: Commit**

```bash
git add src-bthread/
git commit -m "refactor: mechanical namespace and include path replacement (bthread→fast)"
```

---

### Task 7: 替换 C++17 标准库等价物

**Files:**
- Modify: 所有 `src-bthread/*.cpp` 和 `src-bthread/*.h`

**Interfaces:**
- Consumes: 命名空间已修正的文件
- Produces: butil::atomic/butil::Mutex/butil::unique_ptr 等已替换为 std:: 的文件

- [ ] **Step 1: 替换 butil::atomic → std::atomic**

```bash
cd /home/syt/rdma/light-rpc/src-bthread

find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's|#include "butil/atomicops.h"|#include <atomic>|g'
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/butil::atomic/std::atomic/g'
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/butil::memory_order/std::memory_order/g'
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/butil::static_atomic/std::atomic/g'

# 处理 atomicops_internals 相关（删除引用，不再需要）
# 确认所有 Atomic64/AtomicWord 引用已被替换
```

- [ ] **Step 2: 替换 BAIDU_SCOPED_LOCK 和 butil::Mutex**

```bash
cd /home/syt/rdma/light-rpc/src-bthread

# 删除 scoped_lock.h include
find . -name '*.cpp' -o -name '*.h' | xargs sed -i \
    '/#include "butil\/scoped_lock.h"/d'

# BAIDU_SCOPED_LOCK → std::lock_guard（需要确认 mutex 变量类型为 std::mutex）
find . -name '*.cpp' | xargs sed -i \
    's/BAIDU_SCOPED_LOCK(\([^)]*\))/std::lock_guard<std::mutex> _sl_(\1)/g'

# butil::Mutex → std::mutex
find . -name '*.h' -o -name '*.cpp' | xargs sed -i \
    's/butil::Mutex/std::mutex/g'
```

需要手动检查 `task_control.h` 中的 `_modify_group_mutex` 和 `_pending_time_mutex` 声明变为 `std::mutex`。

- [ ] **Step 3: 替换 platform_thread 函数**

```bash
cd /home/syt/rdma/light-rpc/src-bthread

find . -name '*.cpp' | xargs sed -i \
    '/#include "butil\/threading\/platform_thread.h"/d'

# YieldThread() → std::this_thread::yield()
find . -name '*.cpp' | xargs sed -i \
    's/butil::threading::YieldThread()/std::this_thread::yield()/g'
# 同时添加 #include <thread>
```

在 `task_control.cpp` 和 `timer_thread.cpp` 顶部添加 `#include <thread>`。

- [ ] **Step 4: 替换 berror()**

```bash
cd /home/syt/rdma/light-rpc/src-bthread

# 删除 errno.h 中的 butil/errno.h 引用（已在 task 6 处理）
# berror() → strerror(errno)
find . -maxdepth 1 -name '*.cpp' | xargs sed -i \
    's/berror()/strerror(errno)/g'
find . -maxdepth 1 -name '*.cpp' | xargs sed -i \
    's/berror(errno)/strerror(errno)/g'
find . -maxdepth 1 -name '*.cpp' | xargs sed -i \
    '/#include "errno.h"/d'
```

`errno.cpp` 可以删除（berror 只有 2 行代码，全部内联替换为 `strerror(errno)`）。

- [ ] **Step 5: 提交**

```bash
git add src-bthread/
git commit -m "refactor: replace butil with C++17 std (atomic, mutex, thread, unique_ptr)"
```

---

### Task 8: 替换 gflags → FastBthreadConfig + logging 替换

**Files:**
- Modify: `src-bthread/task_group.cpp`, `src-bthread/task_control.cpp`, `src-bthread/stack.cpp`, `src-bthread/bthread.cpp`, `src-bthread/butex.cpp`, `src-bthread/timer_thread.cpp`, `src-bthread/parking_lot.h`

**Interfaces:**
- Consumes: C++17 替换完成后的文件
- Produces: 无 gflags 依赖，FLAGS_* → FastBthreadConfig::Get().*

- [ ] **Step 1: 在每个需要的文件顶部添加 include**

```bash
cd /home/syt/rdma/light-rpc/src-bthread

# 在这些文件中添加 #include "inc/fast_bthread_config.h"
for f in task_group.cpp task_control.cpp stack.cpp bthread.cpp butex.cpp timer_thread.cpp parking_lot.h; do
    sed -i '1s/^/#include "inc\/fast_bthread_config.h"\n/' "$f"
done
```

- [ ] **Step 2: 替换所有 FLAGS_* 引用**

在以下文件中执行全局替换：

```
FLAGS_bthread_concurrency       → FastBthreadConfig::Get().bthread_concurrency
FLAGS_bthread_min_concurrency   → FastBthreadConfig::Get().bthread_min_concurrency
FLAGS_task_group_runqueue_capacity → FastBthreadConfig::Get().task_group_runqueue_capacity
FLAGS_stack_size_small          → FastBthreadConfig::Get().stack_size_small
FLAGS_stack_size_normal         → FastBthreadConfig::Get().stack_size_normal
FLAGS_stack_size_large          → FastBthreadConfig::Get().stack_size_large
FLAGS_guard_page_size           → FastBthreadConfig::Get().guard_page_size
FLAGS_tc_stack_small            → FastBthreadConfig::Get().tc_stack_small
FLAGS_tc_stack_normal           → FastBthreadConfig::Get().tc_stack_normal
FLAGS_task_group_ntags          → FastBthreadConfig::Get().task_group_ntags
FLAGS_bthread_parking_lot_of_each_tag → FastBthreadConfig::Get().parking_lot_of_each_tag
FLAGS_bthread_current_tag       → FastBthreadConfig::Get().bthread_current_tag  (如果不存在则在 config 中补充)
```

```bash
cd /home/syt/rdma/light-rpc/src-bthread

sed -i 's/FLAGS_bthread_concurrency/FastBthreadConfig::Get().bthread_concurrency/g' *.cpp *.h
sed -i 's/FLAGS_bthread_min_concurrency/FastBthreadConfig::Get().bthread_min_concurrency/g' *.cpp *.h
sed -i 's/FLAGS_task_group_runqueue_capacity/FastBthreadConfig::Get().task_group_runqueue_capacity/g' *.cpp *.h
sed -i 's/FLAGS_stack_size_small/FastBthreadConfig::Get().stack_size_small/g' *.cpp *.h
sed -i 's/FLAGS_stack_size_normal/FastBthreadConfig::Get().stack_size_normal/g' *.cpp *.h
sed -i 's/FLAGS_stack_size_large/FastBthreadConfig::Get().stack_size_large/g' *.cpp *.h
sed -i 's/FLAGS_guard_page_size/FastBthreadConfig::Get().guard_page_size/g' *.cpp *.h
sed -i 's/FLAGS_tc_stack_small/FastBthreadConfig::Get().tc_stack_small/g' *.cpp *.h
sed -i 's/FLAGS_tc_stack_normal/FastBthreadConfig::Get().tc_stack_normal/g' *.cpp *.h
sed -i 's/FLAGS_task_group_ntags/FastBthreadConfig::Get().task_group_ntags/g' *.cpp *.h
sed -i 's/FLAGS_bthread_parking_lot_of_each_tag/FastBthreadConfig::Get().parking_lot_of_each_tag/g' *.cpp *.h
```

- [x] **Step 3: 迁移日志宏基础设施并恢复调用点语义（2026-09-27）**

`inc/fast_log.h` 接入从 brpc 移植的 `inc/detail/fast_log_macros.h`：保留惰性求值、CHECK/DCHECK 与原限频算法，继续使用项目输出后端。队列和栈错误恢复限频，调度路径恢复 DCHECK，Release 补回 NDEBUG。

独立日志测试 13 项 × 3 种 DCHECK 构建模式通过；主项目 Debug/Release 构建通过。bthread 核心仍有独立构建阻塞，本步骤不代表完整日志后端迁移或 bthread 集成验证完成。

记录与复现命令见 [日志基础设施修复](bthread-logging-port.md)。完整 bvar 移植暂缓；后续已确认裁剪无消费者的 `_nworkers`、`_nbthreads`，详见 Task 9。其他统计项仍须逐项审核用途。

- [ ] **Step 4: 处理 DEFINE_*/DECLARE_* 语句**

删除所有 `DEFINE_int32`、`DEFINE_bool`、`DECLARE_int32` 等 gflags 语句。如果变量已移入 FastBthreadConfig，删除定义行。

```bash
cd /home/syt/rdma/light-rpc/src-bthread
grep -rn "DEFINE_\|DECLARE_" *.cpp *.h
# 手动删除这些行
```

对于 `BUTIL_VALIDATE_GFLAG` 等 reloadable_flags 宏，直接删除整行。

- [ ] **Step 5: Commit**

```bash
git add src-bthread/
git commit -m "refactor: replace gflags with FastBthreadConfig, fix logging"
```

---

### Task 9: 剥离 bvar 和 valgrind

**Files:**
- Modify: `src-bthread/task_control.h`, `src-bthread/task_control.cpp`, `src-bthread/stack.cpp`
- Modify: `src-bthread/butil/object_pool_inl.h`

**Interfaces:**
- Consumes: gflags 已替换的文件
- Produces: 无 bvar 依赖，valgrind 已 #if 0

- [ ] **Step 1: task_control.h — 删除 bvar include，替换 bvar 成员**

```cpp
// 删除： #include "bvar/bvar.h"
// 删除所有 bvar:: 成员，替换为普通类型

// 原来（task_control.h:149-168）:
bvar::Adder<int64_t> _nworkers;
butil::Mutex _pending_time_mutex;
butil::atomic<bvar::LatencyRecorder*> _pending_time;
bvar::PassiveStatus<double> _cumulated_worker_time;
bvar::PerSecond<bvar::PassiveStatus<double> > _worker_usage_second;
bvar::PassiveStatus<int64_t> _cumulated_switch_count;
bvar::PerSecond<bvar::PassiveStatus<int64_t> > _switch_per_second;
bvar::PassiveStatus<int64_t> _cumulated_signal_count;
bvar::PerSecond<bvar::PassiveStatus<int64_t> > _signal_per_second;
bvar::PassiveStatus<std::string> _status;
bvar::Adder<int64_t> _nbthreads;

// 替换为:
// _nworkers 已裁剪：当前不导出 worker 数量统计。
std::mutex _pending_time_mutex;
std::atomic<int64_t*> _pending_time{nullptr};  // 去掉 bvar::LatencyRecorder，用 int64_t 占位即可
double _cumulated_worker_time{0.0};
int64_t _cumulated_switch_count{0};
int64_t _cumulated_signal_count{0};
// _nbthreads 已裁剪：当前不导出任务数量统计。
// 删除 _worker_usage_second, _switch_per_second, _signal_per_second, _status
// 删除 per-tag bvar vectors: _tagged_nworkers, _tagged_cumulated_worker_time, etc.
```

在 editor 中打开 `task_control.h`，手动完成替换。

- [ ] **Step 2: task_control.cpp — 删除 bvar 初始化代码**

`_nworkers`、`_nbthreads` 的字段和五处生命周期更新已于 2026-09-27 裁剪。原版分别通过 `_nworkers("bthread_worker_count")`、`_nbthreads("bthread_count")` 注册统计指标，对应初始化也不保留。参与调度的 `_concurrency` 等状态不能删除。

删除 `exposed_pending_time()` 方法中创建 `bvar::LatencyRecorder` 的代码。

- [ ] **Step 3: stack.cpp — valgrind #if 0**

```bash
# 包裹 valgrind 相关代码
```

在 `stack.cpp` 中：
- 删除 `#include "butil/third_party/dynamic_annotations/dynamic_annotations.h"` 和 `#include "butil/third_party/valgrind/valgrind.h"`
- 将所有 `RunningOnValgrind()` 调用替换为 `false`
- 将 `VALGRIND_STACK_REGISTER` 和 `VALGRIND_STACK_DEREGISTER` 调用用 `#if 0` 包裹

- [ ] **Step 4: Commit**

```bash
git add src-bthread/
git commit -m "refactor: strip bvar monitoring and valgrind annotations"
```

---

### Task 10: mutex.cpp 大裁剪 + singleton 替换

**Files:**
- Modify: `src-bthread/mutex.cpp`
- Modify: `src-bthread/stack.cpp`, `src-bthread/butex.cpp`

**Interfaces:**
- Consumes: bvar 已剥离的文件
- Produces: mutex.cpp 从 1330→~400 行，singleton 已替换

- [ ] **Step 1: mutex.cpp 加 FAST_MUTEX_NO_CONTENTION 宏守卫**

在 `mutex.cpp` 顶部添加：
```cpp
#define FAST_MUTEX_NO_CONTENTION
```

然后用 `#ifndef FAST_MUTEX_NO_CONTENTION` / `#endif` 包裹以下代码块：

1. ContentionProfiler 相关全局变量和函数（g_cp, g_cp_sl, submit_contention 等）
2. pthread_mutex interposition / dlsym 钩子（所有 `pthread_mutex_*` wrapper）
3. MutexOwner 调试代码
4. bthread::log 相关
5. ContentionProfilerStart/Stop

同时删除以下 include（contention profiling 专用）：
```cpp
#include "butil/iobuf.h"
#include "butil/fd_guard.h"
#include "butil/files/file.h"
#include "butil/files/file_path.h"
#include "butil/file_util.h"
#include "butil/debug/stack_trace.h"
#include "butil/third_party/symbolize/symbolize.h"
#include "butil/memory/scope_guard.h"
#include "butil/unique_ptr.h"
#include "butil/thread_local.h"
#include "butil/object_pool.h"
#include "butil/third_party/murmurhash3/murmurhash3.h"
```

保留的核心代码（~400 行）：
- `MUTEX_CONTENDED_RAW` / `MUTEX_LOCKED_RAW` 常量
- `mutex_lock_contended_impl()` — butex-wait 循环
- `mutex_trylock_impl()` — exchange(locked, acquire)
- `bthread_mutex_init/destroy/trylock/lock/timedlock/unlock` C API
- `bthread_mutexattr_init/destroy/disable_csite` API（简化为空操作）

- [ ] **Step 2: types.h — 移除 csite 字段**

在 `src-bthread/types.h` 中 `bthread_mutex_t` 结构体里删除 `csite` 和 `enable_csite` 字段，同时删除 `bthread_contention_site_t` 类型定义。

```cpp
// 原来:
typedef struct bthread_mutex_t {
    unsigned* butex;
    bthread_contention_site_t csite;
    bool enable_csite;
    mutex_owner_t owner;
} bthread_mutex_t;

// 替换为:
typedef struct bthread_mutex_t {
    unsigned* butex;
    mutex_owner_t owner;
} bthread_mutex_t;
```

- [ ] **Step 3: 替换 singleton_on_pthread_once → C++11 magic static**

在 `stack.cpp` 中：
```cpp
// 原来（get_stack 函数中）:
static StackFactory* factory = SingletonOnPthreadOnce<StackFactory>::LeakySingleton();

// 替换为:
static StackFactory* factory = []() {
    static StackFactory* p = new StackFactory();
    return p;
}();
```

在 `butex.cpp` 中同样替换 `ButexFactory` 的单例模式。

- [ ] **Step 4: Commit**

```bash
git add src-bthread/
git commit -m "refactor: strip mutex contention profiling, replace singleton with C++11 static"
```

---

### Task 11: CMakeLists.txt + 首次编译

**Files:**
- Create: `src-bthread/CMakeLists.txt`
- Modify: `CMakeLists.txt`（主构建文件）

**Interfaces:**
- Consumes: 所有源文件就位
- Produces: 编译目标 `fast_bthread`

- [ ] **Step 1: 读取主 CMakeLists.txt**

```bash
cat /home/syt/rdma/light-rpc/CMakeLists.txt
```

了解现有的 target 命名方式和 include 路径配置。

- [ ] **Step 2: 编写 src-bthread/CMakeLists.txt**

```cmake
set(BTHREAD_SOURCES
    context.cpp
    stack.cpp
    task_group.cpp
    task_control.cpp
    butex.cpp
    timer_thread.cpp
    bthread.cpp
    id.cpp
    key.cpp
    sys_futex.cpp
    mutex.cpp
    condition_variable.cpp
    butil/fast_rand.cpp
    butil/time.cpp
    butil/thread_key.cpp
    butil/third_party/murmurhash3/murmurhash3.cpp
)

add_library(fast_bthread STATIC ${BTHREAD_SOURCES})

target_include_directories(fast_bthread PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(fast_bthread PUBLIC pthread)

target_compile_definitions(fast_bthread PRIVATE
    FAST_MUTEX_NO_CONTENTION
    _GNU_SOURCE
)

target_compile_options(fast_bthread PRIVATE -std=c++17)
```

- [ ] **Step 3: 在主 CMakeLists.txt 中添加子目录**

在主 `CMakeLists.txt` 中添加：
```cmake
add_subdirectory(src-bthread)
```

- [ ] **Step 4: 首次编译**

```bash
cd /home/syt/rdma/light-rpc/build
cmake .. && make -j$(nproc) 2>&1 | tee build.log
```

- [ ] **Step 5: 修复编译错误**

逐个修复 build.log 中的错误。常见错误类型：
- 缺少某个 `#include`（如 `<atomic>`, `<mutex>`, `<thread>`, `<cstring>`）
- 命名空间残留引用（`bthread::` → `fast::`）
- baidu 宏未定义（在 macros.h 精简版中补充）
- 某些 bvar 成员被误删导致引用报错（修改引用代码）

如果修复量很大，分批进行，每修复一类错误 commit 一次。

- [ ] **Step 6: Commit**

```bash
git add src-bthread/CMakeLists.txt CMakeLists.txt src-bthread/
git commit -m "build: add fast_bthread library target, fix compilation errors"
```

---

### Task 12: 单元测试 — 基础 API 验证

**Files:**
- Create: `test/bthread_basic_test.cc`

**Interfaces:**
- Consumes: `fast_bthread` 编译成功
- Produces: 基础功能测试通过

- [ ] **Step 1: 编写基础测试**

```cpp
#include <gtest/gtest.h>
#include "bthread.h"
#include "inc/fast_bthread_config.h"

namespace fast {
namespace {

// 测试 1: bthread_start_background + bthread_join
TEST(BthreadTest, StartAndJoin) {
    static int val = 0;
    void* fn(void*) {
        val = 42;
        return nullptr;
    }
    
    bthread_t tid;
    ASSERT_EQ(0, bthread_start_background(&tid, nullptr, fn, nullptr));
    ASSERT_NE(INVALID_BTHREAD, tid);
    ASSERT_EQ(0, bthread_join(tid, nullptr));
    ASSERT_EQ(42, val);
}

// 测试 2: bthread_self
TEST(BthreadTest, Self) {
    bthread_t tid = bthread_self();
    EXPECT_EQ(0, tid);  // main thread 不是 bthread，返回 0
}

// 测试 3: 多 bthread 并发
TEST(BthreadTest, MultipleBthreads) {
    static std::atomic<int> counter{0};
    void* fn(void*) {
        counter.fetch_add(1);
        return nullptr;
    }
    
    const int N = 100;
    bthread_t tids[N];
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(0, bthread_start_background(&tids[i], nullptr, fn, nullptr));
    }
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(0, bthread_join(tids[i], nullptr));
    }
    ASSERT_EQ(N, counter.load());
}

// 测试 4: bthread_usleep
TEST(BthreadTest, Usleep) {
    auto start = std::chrono::steady_clock::now();
    bthread_usleep(100000);  // 100ms
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_GE(ms, 90);   // 允许 10% 误差
    EXPECT_LE(ms, 150);  // 允许额外 50ms 误差
}

} // namespace
} // namespace fast
```

- [ ] **Step 2: 更新 CMakeLists.txt 添加测试目标**

在主 `CMakeLists.txt` 或 `test/CMakeLists.txt` 中添加：
```cmake
add_executable(bthread_test test/bthread_basic_test.cc)
target_link_libraries(bthread_test fast_bthread pthread gtest gtest_main)
```

- [ ] **Step 3: 编译并运行测试**

```bash
cd build && cmake .. && make bthread_test -j$(nproc)
./bthread_test
```

预期：4 个测试全部 PASS。

- [ ] **Step 4: Commit**

```bash
git add test/bthread_basic_test.cc CMakeLists.txt
git commit -m "test: add basic bthread API unit tests (start, join, self, usleep)"
```

---

### Task 13: 单元测试 — 同步原语

**Files:**
- Create: `test/bthread_sync_test.cc`

- [ ] **Step 1: 编写同步原语测试**

```cpp
#include <gtest/gtest.h>
#include "bthread.h"
#include "inc/fast_bthread_config.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace fast {
namespace {

// butex 基本功能
TEST(ButexTest, WaitWake) {
    unsigned* butex = static_cast<unsigned*>(butex_create());
    ASSERT_NE(nullptr, butex);
    *butex = 0;
    
    std::atomic<bool> woken{false};
    
    void* waiter(void* arg) {
        auto* b = static_cast<unsigned*>(arg);
        butex_wait(b, 0, nullptr);  // 等待 *b == 0
        woken = true;
        return nullptr;
    }
    
    bthread_t tid;
    bthread_start_background(&tid, nullptr, waiter, butex);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(woken.load());
    
    *butex = 1;
    EXPECT_EQ(1, butex_wake(butex));
    
    bthread_join(tid, nullptr);
    EXPECT_TRUE(woken.load());
    butex_destroy(butex);
}

// bthread_mutex 基本功能
TEST(MutexTest, LockUnlock) {
    bthread_mutex_t mutex;
    bthread_mutex_init(&mutex, nullptr);
    
    bthread_mutex_lock(&mutex);
    bthread_mutex_unlock(&mutex);
    
    bthread_mutex_destroy(&mutex);
}

// bthread_mutex 互斥性
TEST(MutexTest, MutualExclusion) {
    bthread_mutex_t mutex;
    bthread_mutex_init(&mutex, nullptr);
    std::atomic<int> counter{0};
    
    void* fn(void* arg) {
        auto* p = static_cast<std::pair<bthread_mutex_t*, std::atomic<int>*>*>(arg);
        for (int i = 0; i < 10000; i++) {
            bthread_mutex_lock(p->first);
            p->second->fetch_add(1);
            bthread_mutex_unlock(p->first);
        }
        return nullptr;
    }
    
    std::pair<bthread_mutex_t*, std::atomic<int>*> arg{&mutex, &counter};
    bthread_t t1, t2;
    bthread_start_background(&t1, nullptr, fn, &arg);
    bthread_start_background(&t2, nullptr, fn, &arg);
    bthread_join(t1, nullptr);
    bthread_join(t2, nullptr);
    
    EXPECT_EQ(20000, counter.load());
    bthread_mutex_destroy(&mutex);
}

// bthread_cond 基本功能
TEST(CondTest, WaitSignal) {
    bthread_mutex_t mutex;
    bthread_cond_t cond;
    bthread_mutex_init(&mutex, nullptr);
    bthread_cond_init(&cond, nullptr);
    std::atomic<bool> signaled{false};
    
    void* waiter(void* arg) {
        auto* p = static_cast<std::tuple<bthread_mutex_t*, bthread_cond_t*, std::atomic<bool>*>*>(arg);
        bthread_mutex_lock(std::get<0>(*p));
        while (!std::get<2>(*p)->load()) {
            bthread_cond_wait(std::get<1>(*p), std::get<0>(*p));
        }
        bthread_mutex_unlock(std::get<0>(*p));
        return nullptr;
    }
    
    std::tuple<bthread_mutex_t*, bthread_cond_t*, std::atomic<bool>*> arg{&mutex, &cond, &signaled};
    
    bthread_t tid;
    bthread_start_background(&tid, nullptr, waiter, &arg);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(signaled.load());
    
    signaled = true;
    bthread_cond_signal(&cond);
    bthread_join(tid, nullptr);
    
    bthread_cond_destroy(&cond);
    bthread_mutex_destroy(&mutex);
}

} // namespace
} // namespace fast
```

- [ ] **Step 2: 编译并运行**

```bash
cd build && make bthread_test -j$(nproc)
./bthread_test
```

- [ ] **Step 3: Commit**

```bash
git add test/bthread_sync_test.cc CMakeLists.txt
git commit -m "test: add bthread sync primitive unit tests (butex, mutex, cond)"
```

---

### Task 14: 清理 — 删除 errno.cpp 和未使用文件

- [ ] **Step 1: 删除 errno.cpp**

```bash
rm /home/syt/rdma/light-rpc/src-bthread/errno.cpp
```

berror() 已在 Task 7 中全部内联替换为 strerror(errno)，这个 cpp 不再需要。

- [ ] **Step 2: 更新 CMakeLists.txt**

从 `BTHREAD_SOURCES` 中移除 `errno.cpp`。

- [ ] **Step 3: 最终全量编译验证**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -5
# 预期：无编译错误
./bthread_test
# 预期：所有测试 PASS
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore: remove unused errno.cpp, final cleanup"
```

---

## 验证 CHECKLIST

移植完成后执行：

```bash
cd /home/syt/rdma/light-rpc/build

# 1. 编译验证
cmake .. && make -j$(nproc)
# 预期：0 errors

# 2. 单元测试
./bthread_test
# 预期：9 个测试全部 PASS

# 3. 确认无外部依赖
ldd libfast_bthread.a
# 预期：无 gflags, protobuf, unwind 链接
```

---

## 目录最终状态

```
src-bthread/
├── context.h
├── context.cpp
├── stack.h
├── stack_inl.h
├── stack.cpp
├── task_meta.h
├── task_group.h
├── task_group_inl.h
├── task_group.cpp
├── task_control.h
├── task_control.cpp
├── work_stealing_queue.h
├── remote_task_queue.h
├── parking_lot.h
├── butex.h
├── butex.cpp
├── timer_thread.h
├── timer_thread.cpp
├── bthread.h
├── bthread.cpp
├── types.h
├── id.h
├── id.cpp
├── list_of_abafree_id.h
├── errno.h
├── key.cpp
├── processor.h
├── prime_offset.h
├── sys_futex.h
├── sys_futex.cpp
├── mutex.h
├── mutex.cpp
├── condition_variable.h
├── condition_variable.cpp
├── CMakeLists.txt
└── butil/
    ├── macros.h
    ├── build_config.h
    ├── aligned_memory.h
    ├── fast_rand.h
    ├── fast_rand.cpp
    ├── time.h
    ├── time.cpp
    ├── thread_local.h
    ├── thread_local_inl.h
    ├── thread_key.h
    ├── thread_key.cpp
    ├── flat_map.h
    ├── linked_list.h
    ├── bounded_queue.h
    ├── resource_pool.h
    ├── resource_pool_inl.h
    ├── object_pool.h
    ├── object_pool_inl.h
    └── third_party/
        └── murmurhash3/
            ├── murmurhash3.h
            └── murmurhash3.cpp
```
