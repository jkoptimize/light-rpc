# butil/thread_key.{h,cpp} — ThreadKey 与 ThreadLocal<T>

## 1. 作用概述

`thread_key` 模块是 **bthread TLS 的底层基础**，提供两层抽象：

1. **`ThreadKey` + `thread_key_create/delete/setspecific/getspecific`**：一个**没有数量上限**的 `pthread_key` 替代品。原生 `pthread_key_create` 受 `PTHREAD_KEYS_MAX`（通常 1024）限制，而 bthread 需要为每个用户 key 池、每个协程上下文动态创建 key，数量不可预知。`ThreadKey` 用「全局 `std::vector<ThreadKeyInfo>` + 单调递增 id + 序列号（seq）防 ABA + 空闲 id 复用池（`std::deque`）」实现无上限的 key 分配。
2. **`ThreadLocal<T>`**：在 `ThreadKey` 之上的高级模板，语义类似 `boost::thread_specific_ptr`——每个线程惰性构造一个 `T`，支持 `get()`/`reset()`/`for_each()`，线程退出或 `ThreadLocal` 析构时统一释放。

核心数据结构：
- `ThreadKey { _id, _seq }`：`_id` 是 key 槽位索引，`_seq` 是防 ABA 的版本号（每次 create/delete 递增，用最低位标记「已分配/未使用」）。
- `ThreadKeyInfo { seq, dtor }`：全局表中每个槽位记录的「当前版本 + 析构函数」。
- `ThreadKeyTLS { seq, data }`：每个线程的 `__thread` TLS 向量，`thread_getspecific` 时用 `seq` 校验 key 是否仍有效（防止拿到被 delete 后复用的旧数据）。

## 2. 关键 API

| 符号 | 说明 |
|------|------|
| `ThreadKey` | key 句柄，`_id` + `_seq`，可移动、不可拷贝 |
| `thread_key_create(ThreadKey&, DtorFunction)` | 分配一个 key（无数量上限），返回 0 / `EAGAIN` |
| `thread_key_delete(ThreadKey&)` | 释放 key，id 归还空闲池 |
| `thread_setspecific(ThreadKey&, void*)` | 写入当前线程的 TLS 槽位 |
| `thread_getspecific(ThreadKey&)` | 读取当前线程的 TLS 槽位（seq 校验） |
| `ThreadKeyInfo` / `ThreadKeyTLS` | 内部：全局表项 / 线程 TLS 项 |
| `ThreadLocal<T>` | 高级 TLS 模板：`get()`/`operator->`/`operator*`/`reset()`/`for_each()` |

## 3. 在 bthread 中的作用

`ThreadLocal<T>` 被 bthread 的 **`key.cpp`（bthread 用户级 TLS 实现）** 直接使用：

```cpp
// key.cpp — bthread 的 KeyTable 池
pool->list = new butil::ThreadLocal<bthread::KeyTableList>();
...
auto list = (butil::ThreadLocal<bthread::KeyTableList>*)pool->list;
list->get()->append(kt);              // 往当前线程的 key table 追加
KeyTable* result = list->get()->remove_front();
```

作用链：

1. `bthread_key_create/delete/setspecific/getspecific`（bthread 对用户暴露的协程级 TLS API）底层需要一个 **per-thread 的 `KeyTableList`**——记录该线程（及其协程）创建的所有 key 槽位。
2. 这个 `KeyTableList` 用 `ThreadLocal<KeyTableList>` 存储：每个 worker 线程惰性构造自己的 key table 列表，互不干扰。
3. 于是 `ThreadLocal` 的「无上限 key + 线程惰性构造 + 退出释放」正好满足 bthread TLS 对「海量动态 key、多 worker 线程隔离」的需求。

`ThreadKey` 本身虽在 bthread 核心里 0 次**直接**调用，但它是 `ThreadLocal<T>::get()`/`reset()` 的底层（内部调用 `thread_setspecific`/`thread_getspecific`），因此是 `ThreadLocal` 的**间接依赖**，必须一起移植。

## 4. 移植裁剪决策

| 原文件内容 | 决策 | 理由 |
|------------|------|------|
| `#include "butil/scoped_lock.h"` | **删除**，改用 `macros.h` 的 `BAIDU_SCOPED_LOCK` | scoped_lock.h 不移植 |
| `#include "butil/type_traits.h"`（`is_result_void`） | **删除**，改用 `std::is_void<std::invoke_result_t<Callback, T*>>` | type_traits.h 不移植，C++17 原生等价 |
| `pthread_mutex_t _mutex` / `g_thread_key_mutex` | **改为 `std::mutex`** | butil::Mutex → std::mutex |
| `_mutex(PTHREAD_MUTEX_INITIALIZER)` | **删除初始化项** | std::mutex 默认构造 |
| `pthread_mutex_destroy(&_mutex)` | **删除** | std::mutex 自动析构 |
| `butil::thread_atexit(DestroyTlsData)` | **改为非限定 `thread_atexit(...)`** | 已在 `fast::butil` 命名空间内 |
| `#include "butil/thread_local.h"` | **改为 `"thread_local.h"`** | 相对路径 |

新增依赖：`<mutex>`、`<type_traits>`、`<new>`（`std::nothrow`）、`<algorithm>`（`std::remove`）、`<errno.h>`（`EAGAIN`/`EINVAL`）。

## 5. 与 std 的关系

C++11 的 `thread_local` 关键字看似能替代 `ThreadLocal<T>`，但两者语义不同：

1. **`ThreadLocal<T>` 是「运行时 key」而非「编译期变量」**：`ThreadKey` 的 key 是运行时分配、可动态创建/销毁、无数量上限，这是 `thread_local` 关键字（编译期声明、数量即代码量）无法表达的。bthread 需要在运行时为任意数量的用户 key 池分配 TLS 槽位。
2. **防 ABA 的 seq 机制**：`ThreadLocal` 用 seq 号保证 `get`/`delete` 交错时不会读到已释放 key 的陈旧数据，这是裸 `thread_local` 没有的语义。
3. **跨线程遍历 `for_each`**：`ThreadLocal` 记录了「所有线程分配过的对象指针」`ptrs`，支持 `for_each` 遍历做统一清理，`thread_local` 无法枚举其他线程的实例。

因此 `ThreadKey`/`ThreadLocal` 是针对「动态、海量、需防 ABA、需跨线程管理」的 TLS 场景设计，与 `thread_local` 是互补而非替代关系。
