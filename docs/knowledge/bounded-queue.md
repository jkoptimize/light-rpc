# BoundedQueue：固定容量的环形双端队列

> 源码：[butil/bounded_queue.h](../../src-bthread/butil/bounded_queue.h)。

## 功能与并发约束

支持两端插入、删除，但**不是线程安全的无锁队列**。内部没有锁，也没有原子操作；单线程可直接使用，多线程共享时需要调用者提供同步。

| 接口 | 操作 |
|------|------|
| `push` | 尾部（bottom）插入，满则失败 |
| `push_top` | 头部（top）插入，满则失败 |
| `pop` | 头部删除，空则失败 |
| `pop_bottom` | 尾部删除，空则失败 |
| `elim_push` | 尾部插入；满时覆盖最旧的头部元素，并推进头部 |
| `top(i)` / `bottom(i)` | 从对应端按逻辑位置访问，越界返回空指针 |

`push + pop` 是 FIFO 队列用法；`push + pop_bottom` 是 LIFO 栈用法。注意这里 `pop` 默认取头部，与 WorkStealingQueue 的 owner pop 取尾部不同。

## 大体实现

底层是一块固定的连续存储，使用三个普通整数维护状态：

- `_cap`：容量；不自动扩容，也不要求是 2 的幂。
- `_start`：头部在数组中的位置。
- `_count`：现有元素数量；0 表示空，等于容量表示满。

第 i 个逻辑元素位于 `(_start + i) mod _cap`。源码用 `_mod` 减法回绕，不移动其余元素。因此底层内存连续，但跨数组末尾时，逻辑元素可能分成前后两段。

尾插在 `(_start + _count) mod _cap` 构造对象并增加计数；头插先让 `_start` 向前回绕。头删析构对象后推进 `_start`；尾删减少计数后析构对应对象。正常两端操作的索引维护是 O(1)，元素自身的构造、复制与析构成本另计。

存储可由构造函数 malloc，也可由调用者提供（需满足 T 的大小和对齐要求）。通过 placement new 构造元素、显式调用析构函数；析构队列时清理元素，仅在 `OWNS_STORAGE` 时 free 底层内存。`elim_push` 满队列分支直接赋值覆盖对象；该操作要求有效的正容量。

## 在 bthread 中的用途

[RemoteTaskQueue](../../src-bthread/remote_task_queue.h) 用它保存外部提交的 `bthread_t`，在 push 和实际 pop 操作外加 mutex；`push_locked` 要求调用者已持锁。不能因为内部容器没有锁，就把远程队列理解为无锁队列。

## 自测

为什么只有 `_start + _count` 就能定位尾部？为什么底层连续存储不代表所有有效元素总是一段连续区间？为什么“没有锁”不等于“支持无锁并发”？
