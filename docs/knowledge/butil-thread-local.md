# butil/thread_local.{h,inl,cpp} — 线程局部存储与线程退出回调

## 1. 作用概述

`thread_local` 模块提供两件事：

1. **`BAIDU_THREAD_LOCAL` / `BAIDU_VOLATILE_THREAD_LOCAL` 宏族**：基于 GCC `__thread` 的线程局部变量声明，以及为 aarch64/clang 编译器 bug 设计的「防优化」访问器（`get_xxx()` / `set_xxx()` / `get_ptr_xxx()`）。
2. **`thread_atexit()` / `thread_atexit_cancel()`**：**线程退出时的回调机制**——允许注册函数，在线程结束时按 **LIFO** 顺序调用（若是主线程，则在进程退出时调用），用于释放 `__thread` 声明的资源。

核心实现是 `detail::ThreadExitHelper`：一个挂在 `pthread_key`（`pthread_key_create` + `pthread_getspecific`/`setspecific`）上的、持有 `std::vector<std::pair<Fn, void*>>` 的对象。每个线程首次调用 `thread_atexit` 时惰性创建该 helper，线程退出时由 `pthread_key` 的析构回调 `delete_thread_exit_helper` 触发 `~ThreadExitHelper()`，逆序执行所有已注册函数。

## 2. 关键 API

| 符号 | 说明 |
|------|------|
| `BAIDU_THREAD_LOCAL` | `__thread` 别名（删除了 MSVC 的 `__declspec(thread)` 分支） |
| `BAIDU_VOLATILE_THREAD_LOCAL(type, name, init)` | 声明 `__thread` 变量 + 生成 `get_name()`/`get_ptr_name()`/`set_name()` 三个 `noinline` 访问器 |
| `STATIC_MEMBER_BAIDU_VOLATILE_THREAD_LOCAL(type, name)` | 同上，但用于类静态成员 |
| `BAIDU_GET/SET/PTR_VOLATILE_THREAD_LOCAL(name)` | 访问器调用（aarch64/clang 下走函数，其余平台直接读写） |
| `EXTERN_BAIDU_VOLATILE_THREAD_LOCAL(type, name)` | 跨文件 extern 声明 |
| `thread_atexit(fn)` / `thread_atexit(fn, arg)` | 注册线程退出回调，LIFO，返回 0/-1 |
| `thread_atexit_cancel(fn)` / `thread_atexit_cancel(fn, arg)` | 取消已注册回调 |
| `get_thread_local<T>()` | 获取线程局部 `T` 对象（首次调用默认构造，线程退出自动 delete） |
| `delete_object<T>(arg)` | `thread_atexit` 用的辅助：`delete static_cast<T*>(arg)` |
| `detail::ThreadExitHelper` | 核心：持有回调列表，析构时逆序执行 |

## 3. 在 bthread 中的作用

### 3.1 `tls_task_group` — 调度器的「当前协程上下文」指针

最关键的一处使用：

```cpp
// task_group.cpp
BAIDU_VOLATILE_THREAD_LOCAL(TaskGroup*, tls_task_group, NULL);

// 其他文件跨 TU 读取
EXTERN_BAIDU_VOLATILE_THREAD_LOCAL(TaskGroup*, tls_task_group);
TaskGroup* g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);  // 18 处热路径
```

`tls_task_group` 保存**当前 OS 线程正在执行的 `TaskGroup`**（worker 线程的调度上下文）。bthread 的 `bthread_self()`、`bthread_yield()`、butex/mutex 的阻塞挂起、TLS 读写，全部靠这个 `__thread` 变量定位「我现在在哪个调度组里」。它是 M:N 调度器把「OS 线程」映射到「协程运行上下文」的枢纽。

之所以用 `BAIDU_VOLATILE_THREAD_LOCAL` 而非裸 `__thread`：协程上下文切换会跨越编译器认为的「顺序点」，aarch64 的 GCC 和 Clang 会错误地缓存 `__thread` 变量的地址，导致切换后读到旧值。`noinline` 访问器 + `asm volatile` 强制每次真正从 TLS 读取。

### 3.2 `thread_atexit` — 线程退出时清理 TLS 资源

- `bthread.cpp:154`：`thread_atexit([]() { ... })` 注册 worker 线程退出时的清理逻辑。
- `key.cpp:642`：`thread_atexit(bthread::cleanup_pthread, kt)` 注册每个 pthread 的 bthread-key 清理函数。
- `thread_key.cpp`（butil）：`thread_setspecific` 首次创建 TLS 数据时，用 `thread_atexit(DestroyTlsData)` 注册该线程的 TLS 销毁回调。

这些资源用 `__thread` 声明（比 `pthread_getspecific` 快得多），但 `__thread` 无法挂析构函数，因此靠 `thread_atexit` 在线程退出时统一释放。

## 4. 移植裁剪决策

| 原文件内容 | 决策 | 理由 |
|------------|------|------|
| `#ifdef _MSC_VER` 的 `__declspec(thread)` 分支 | **删除** | build_config.h 保证 GCC/Clang |
| `#include "butil/macros.h"` | **改为 `"macros.h"`** | 相对路径 |
| `butil::thread_atexit(...)`（thread_local_inl.h 内） | **改为非限定 `thread_atexit(...)`** | 已在 `fast::butil` 命名空间内，原 `butil::` 前缀会解析失败 |
| 其他内容 | **保留** | `ThreadExitHelper`、`BAIDU_VOLATILE_THREAD_LOCAL` 系列宏、`get_thread_local` 全部是 bthread 核心依赖 |

**无内容删除**：`thread_atexit`/`thread_atexit_cancel`/`get_thread_local`/宏族全部被 bthread 使用（`get_thread_local` 虽当前 0 次直接调用，但它是 `thread_local_inl.h` 的标准接口，与 `delete_object`/`ThreadLocalHelper` 构成完整工具集，保留成本极低）。

## 5. 与 std 的关系

C++11 的 `thread_local` 关键字和 `std::atexit` 看似能替代，但保留 `butil/thread_local` 的理由：

1. **`thread_local` 无法表达「线程退出析构」语义**：C++ `thread_local` 对象的析构在隐式生成的线程退出路径上执行，但无法注册任意「函数回调」来释放 `__thread` 裸指针资源（如 `tls_task_group` 指向的堆对象）。`thread_atexit` 正是为 `__thread` 声明的资源提供析构挂载点。
2. **`__thread` 快于 `pthread_getspecific`/`thread_local` 对象**：`tls_task_group` 在调度热路径被读 18 次，用 `__thread`（单条 TLS 偏移指令）而非 `pthread_key` 查表，是性能关键。
3. **`BAIDU_VOLATILE_THREAD_LOCAL` 是必要的编译器 workaround**：`std::thread_local` 没有对应的「禁用跨挂起点地址缓存优化」机制，无法解决 aarch64/clang 的上下文切换 bug。
