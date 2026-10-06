# 项目知识点记录

记录阅读本项目源码时形成的理解、推导和修正。以当前移植代码为依据，区分代码事实、帮助理解的简化模型和未验证的假设。

## 索引

### bthread / 同步原语

| 知识点 | 主要源码 | 内容 |
|--------|----------|------|
| [WorkStealingQueue 的 push/pop/steal](work-stealing-queue.md) | `src-bthread/work_stealing_queue.h` | owner 与 thief 的分工、最后一个元素的 CAS 竞争、内存序与失败语义 |
| [BoundedQueue 环形双端队列](bounded-queue.md) | `src-bthread/butil/bounded_queue.h` | 双端操作、环形下标、存储所有权与外部同步 |
| [ParkingLot 等待与唤醒](parking-lot.md) | `src-bthread/parking_lot.h` | 空闲 worker 休眠、通知状态与防止错过唤醒 |
| [VersionedRefWithId 版本化引用与回收竞争](versioned-ref-with-id.md) | `inc/versioned_ref_with_id.h` | 版本/引用计数编码、两个回收者的 CAS 竞争、`ver1+1==ver2` 兜底回收 |
| [brpc mutex 基准实测](brpc-mutex-benchmark.md) | brpc `test/bthread_mutex_unittest.cpp` | 三种锁在 pthread / bthread 中的原版竞争基准、结果与限制 |
| [mutex 分层与 FastPthreadMutex 移植](brpc-mutex-hooks.md) | `src-bthread/mutex.h`、`mutex.cpp`，brpc pthread hook | timedlock 宏、sys_pthread 函数指针、基准解读及 futex 实现移植范围 |

### butil 工具模块

| 知识点 | 主要源码 | 内容 |
|--------|----------|------|
| [AlignedMemory 对齐存储单元](butil-aligned-memory.md) | `src-bthread/butil/aligned_memory.h` | 对齐分配与手动构造 |
| [fast_rand 快速随机数](butil-fast-rand.md) | `src-bthread/butil/fast_rand.*` | 快速随机数生成 |
| [flat_map 首节点内联哈希表](butil-flat-map.md) | `src-bthread/butil/flat_map.h` | 相比 `std::unordered_map` 的性能优势与实现权衡 |
| [ThreadKey / ThreadLocal](butil-thread-key.md) | `src-bthread/butil/thread_key.*` | 线程私有数据的 API 与实现 |
| [thread_local 线程局部存储](butil-thread-local.md) | `src-bthread/butil/thread_local.*` | 线程局部存储与线程退出回调 |
| [time 时间测量工具](butil-time.md) | `src-bthread/butil/time.*` | 时间测量 API |

### 设计权衡与移植复盘

| 知识点 | 主要源码 | 内容 |
|--------|----------|------|
| [TLS/内存模块与 pthread 的对比](butil-thread-key-vs-pthread.md) | `butil/thread_key`、`thread_local`、`aligned_memory` | 为什么重写 pthread_key、ManualConstructor 的动机等设计权衡 |
| [bthread 移植偏差检查](bthread-port-review.md) | `src-bthread/` | mutex 之外的移植偏差复盘 |
| [bthread 基础设施复核（2026-10-06）](bthread-port-audit-2026-10-06.md) | `src-bthread/`、`inc/fast_bthread_config.h` | brpc 逐模块比对、启动校验修复、支持范围与 Debug/Release 回归 |

## 记录约定

每篇记录包含问题背景、源码位置、实现模型、关键竞争过程、容易误解的地方和自测问题。源码位置以函数名为主；并发时序示例用于解释指定场景，不代替完整的内存模型正确性证明。

学习/移植计划见 [docs/works](../works/)。
