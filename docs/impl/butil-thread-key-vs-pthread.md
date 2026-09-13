# butil TLS 与内存模块的设计权衡 — 与 pthread 的对比

> 本文是对 `butil/thread_key`、`butil/thread_local`、`butil/aligned_memory` 三个模块的**设计动机**补充说明，聚焦「为什么 brpc 重写了 pthread_key / 为什么要单独一个 ManualConstructor」这类问题，以及讨论过程中沉淀下来的认知修正。各模块的「是什么 / API / 裁剪决策」见同目录下 `butil-thread-key.md`、`butil-thread-local.md`、`butil-aligned-memory.md`。

---

## 1. 为什么重写 pthread_key：唯一的硬理由是「数量上限」

`thread_key`（`ThreadKey` + `thread_key_create/delete/setspecific/getspecific`）重写了 pthread 的同名能力。逐条审视「重写理由」，**真正站得住脚的只有一条**：

| 候选理由 | 结论 |
|---------|------|
| **key 数量无上限** | ✅ 硬理由（见下） |
| seq 防 ABA | ❌ 不是「pthread 给不了」——glibc 内部就有 seq，brpc 是**复刻** |
| 性能（`__thread` 下标 vs 查表） | ⚠️ 是优化，不是必要性 |

pthread_key 受 `PTHREAD_KEYS_MAX` 硬限制（POSIX 最低 128、glibc 1024，编译期常量，绕不过去）。而 `thread_key` 把上限推到 `size_t` 范围：

```cpp
// thread_key.cpp
static size_t g_id = 0;                       // 单调递增 id
static std::deque<size_t>* g_free_ids = NULL; // 空闲 id 复用池

if (g_id >= ThreadKey::InvalidID) {           // InvalidID = size_t 最大值
    return EAGAIN;                            // 只有 size_t 溢出才报错
}
```

`thread_key` 是 `ThreadLocal<T>` 的底层，而 `ThreadLocal<T>` 是**暴露给用户的通用 TLS API**——每个 `ThreadLocal<T>` 实例 = 一个 key，大型服务里 key 数量不可预知，1024 会轻易突破。

**关于 seq 的诚实定位**：glibc 内部就是 `struct pthread_key_data { uintptr_t seq; void* data; }` + `struct pthread_key_struct { uintptr_t seq; void (*destr)(void*); }`，create/delete 时 `++seq`、getspecific 比较 seq。brpc 的 `thread_key.cpp`（`KEY_UNUSED`、create/delete `++seq`、getspecific 比较 seq）是**复刻这套机制**，因为绕过 pthread_key 后必须自己补上，否则丢 ABA 防护。

---

## 2. 关键区分：thread_key 是「多槽位」，thread_atexit 是「多回调」

「全局单 key」这个规避数量上限的手法，**能救 thread_atexit，救不了 thread_key**——因为两者的 key 使用模式根本不同：

| | thread_atexit | thread_key |
|---|---|---|
| 需求 | N 个**退出回调** | N 个**独立 TLS 槽位** |
| 能否共享单 key | ✅ 回调不需要「按 key 独立寻址」，只需「退出时批量执行」 | ❌ 每个 key 必须「独立 setspecific/getspecific」 |
| key 使用 | 1 个 key 复用 | N 个 key **同时存活、各自独立** |

`thread_key` 的每个 `ThreadKey` 就是一个独立数据槽位——线程对 key=3 存 data_X、对 key=7 存 data_Y，两槽位必须同时、独立存在，**无法合并成一个全局 key**（单 key 只能存一份 data）。因此「无上限」只能靠「动态 vector + 递增 id」，不能靠「单 key 复用」。

---

## 3. thread_atexit：pthread_key 之上的薄封装

### 3.1 底层本来就是 pthread_key

`thread_atexit` 不是「绕开 pthread_key」，而是「**基于 pthread_key 的薄封装**」：

```cpp
// thread_local.cpp:69
static pthread_key_t thread_atexit_key;   // 全局唯一一个 pthread_key
```

「线程退出时执行清理」这个能力 pthread_key **本来就有**，`thread_atexit` 只是借它的 dtor 来触发。它相对「裸 pthread_key」的真正增量只有三点：

| 能力 | pthread_key 裸用 | thread_atexit |
|------|-----------------|---------------|
| 线程退出清理 | ✅ 有 | ✅ 有（底层就靠 pthread_key） |
| key 消耗 | N 回调 = N key | **N 回调 = 1 key**（全局复用） |
| 回调顺序 | unspecified | **LIFO** |
| 动态取消 | ❌ | ✅ `thread_atexit_cancel` |

所以 thread_atexit 的本质是「**全局单 key + 类型擦除的回调列表**」（`ThreadExitHelper` 里的 `std::vector<pair<Fn, void*>>`），增量是「节省 key + LIFO + 可取消」，**不是**「提供了 pthread_key 没有的线程退出能力」。

### 3.2 LIFO 是「回调顺序」，不是「setspecific 变量顺序」

`thread_key` 内部多个 key 的 dtor 顺序**不是 LIFO**，而是 **key id 升序**（`DestroyTlsData` 里 `for (i = 0; i < size; ++i)`）。LIFO 只体现在 `thread_atexit` 的回调层面：`ThreadExitHelper` 析构时从 `_fns.back()` 逆序弹出。

### 3.3 LIFO 的必要性：`pthread_fake_meta` vs `cleanup_pthread`

一个 pthread 典型生命周期中会注册两个 `thread_atexit` 回调：

```cpp
// ① bthread.cpp:154 —— 先注册，清理 pthread_fake_meta（TaskMeta）
butil::thread_atexit([]() {
    ...
    butil::return_resource(get_slot(pthread_fake_meta->tid));
    pthread_fake_meta = NULL;   // 注意：置 NULL
});

// ② key.cpp:642 —— 后注册，cleanup_pthread 清理 KeyTable
butil::thread_atexit(bthread::cleanup_pthread, kt);
// cleanup_pthread 里 delete kt，会触发每个 key 的 data 的用户 dtor
```

LIFO 保证：**cleanup_pthread（后注册）先执行，①（先注册）后执行**。因为 `cleanup_pthread` 的 `delete kt` 触发的用户 dtor 可能调用 `bthread_self()`，而 `bthread_self()` 在 pthread 场景直接读 `pthread_fake_meta`——它必须还活着。

**关键认知修正**：这里 LIFO 的价值是「**避免清理阶段的重入与语义错乱**」，**不是「防止 use-after-free」**。因为 `pthread_fake_meta = NULL`（bthread.cpp:175）已经兜底了：即使顺序反了，`bthread_self()` 看到 NULL 会走重新分配分支，不会解引用已释放的旧 TaskMeta。所以准确的表述是：

- **`pthread_fake_meta = NULL`** 是**兜底**——顺序错了也不至于 UB（但代价是退出阶段重入 `get_resource` 分配、dtor 拿到虚假 tid）。
- **LIFO** 是**主保证**——让清理按正确顺序，从根本上避免触发兜底路径。

---

## 4. ThreadLocal 的清理策略：`delete_on_thread_exit`

`ThreadLocal<T>` 用 `delete_on_thread_exit` 参数在「线程退出」和「对象析构」两个事件里选一个作为 data 清理时机：

| | `delete_on_thread_exit = true` | `delete_on_thread_exit = false`（默认） |
|---|---|---|
| 构造时 | `thread_key_create(_key, DefaultDtor)` 注册 dtor | `thread_key_create(_key, NULL)` 不注册 |
| 线程退出时 | dtor 删 data（各线程清自己） | 不清理，data 变孤儿 |
| 对象析构时 | 跳过 ptrs 遍历（防 double-free） | 遍历 `ptrs` 统一 delete |

**默认 `false` 的动机**：`true` 依赖「pthread 退出触发 dtor」，但 bthread 是 M:N 协程，**worker pthread 几乎从不退出**（固定池常驻），bthread 退出 ≠ pthread 退出，dtor 永远等不到 → 泄漏。所以把清理时机绑在「确定会发生的对象析构」，用 `ptrs`（加 `_mutex` 保护）登记所有线程分配过的指针。

**`ptrs` 存在的意义**：呼应第 2 节的「多槽位」困境——底层 `thread_key` 在 delete 时**访问不到其他线程的 `__thread` TLS**，`ThreadLocal` 析构要清理「所有线程的 data」，只能在 `get()` 时把每个线程的指针额外登记进共享 `ptrs`，析构时遍历清理。这是 `ThreadLocal` 层在 `thread_key` 之上补的能力。

**`true` 的适用场景**：真正的 pthread + 线程频繁创建/销毁。此时若用 `false`，线程 churn 会让 `ptrs` 无限累积孤儿指针（延迟泄漏）。

---

## 5. AlignedMemory vs ManualConstructor：类型擦除 vs 类型绑定

两者都是 Chromium 的 POD 存储组件，为何不合并成一个类？核心是**模板参数的语义根本不同**：

```cpp
// AlignedMemory —— 尺寸参数化，类型被「擦除」成两个数字
typedef AlignedMemory<sizeof(T), __alignof__(T)> BlockItem;   // object_pool_inl.h:124

// ManualConstructor —— 类型参数化，保留类型信息
ManualConstructor<Element> element_space_;                     // flat_map.h:316
```

`AlignedMemory<sizeof(T), __alignof__(T)>` 里的 `sizeof(T)` / `__alignof__(T)` 是**编译期算出的两个 `size_t` 值**，`T` 在类型层面已不存在——AlignedMemory「不知道」自己存的是 T。而 `get()` / `Init()` / `Destroy()` 恰恰**必须知道 T**（返回 `T*`、构造/析构 T）。

所以矛盾在于：**一个「不知道类型」的类，无法提供「类型化的 get/Init/Destroy」。**

`AlignedMemory` 刻意保持「尺寸参数化」，是为了让 object_pool / resource_pool 做**类型无关的内存管理**（全局链表操作「对齐字节块」，只关心尺寸对齐，不关心 T，只有 `get_object<T>()` / `data_as<T>()` 才类型化访问）。若强行塞入 `get/Init/Destroy`，就必须让它绑定 T（从 `<Size, Align>` 改成 `<Type>`），**丢失类型擦除**，pool 的「类型无关 block 池」设计被破坏。

两者的分工是正交的：

- **AlignedMemory**：解决「**对齐存储**」（平台相关的 alignas 特化、POD、可进 union）。
- **ManualConstructor**：解决「**手动生命周期**」（何时构造/析构、复用内存，small_map.h:574 注释 "call constructors and destructors manually, but don't want to allocate/deallocate memory separately"）。

ManualConstructor 在 brpc 里并非死代码，被 4 个容器使用：`flat_map.h:316`、`small_map.h:586`、`optional.h:405`、`mpsc_queue.h:36`（其中 `flat_map` 是 thread_key 的依赖）。

---

## 6. 讨论中沉淀的关键认知修正

1. **seq 防 ABA 不是 pthread 给不了的**：glibc 内部就有 seq，brpc 是复刻（见 §1）。
2. **thread_atexit 底层就是 pthread_key**：它不是「替代品」而是「薄封装」，增量是 LIFO + 可取消 + 单 key 复用（见 §3.1）。
3. **「全局单 key」不能救 thread_key**：thread_key 是「多槽位必须多 key」，与 thread_atexit 的「多回调复用单 key」本质不同（见 §2）。
4. **LIFO 的价值是「避免重入与语义错乱」，不是「防 use-after-free」**：后者由 `pthread_fake_meta = NULL` 兜底（见 §3.3）。
5. **delete 时无法跨线程清理是物理限制，不是性能选择**：`__thread` 变量每线程私有、互相不可达，POSIX 语义也规定 `pthread_key_delete` 不清理残留数据；seq 让残留 data「逻辑失效」，真正的释放发生在各线程退出时（`DestroyTlsData`）。
