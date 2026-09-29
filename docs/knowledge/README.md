# 项目知识点记录

记录阅读本项目源码时形成的理解、推导和修正。以当前移植代码为依据，区分代码事实、帮助理解的简化模型和未验证的假设。

## 索引

| 知识点 | 主要源码 | 内容 |
|--------|----------|------|
| [WorkStealingQueue 的 push/pop/steal](work-stealing-queue.md) | `src-bthread/work_stealing_queue.h` | owner 与 thief 的分工、最后一个元素的 CAS 竞争、内存序与失败语义 |
| [BoundedQueue 环形双端队列](bounded-queue.md) | `src-bthread/butil/bounded_queue.h` | 双端操作、环形下标、存储所有权与外部同步 |
| [ParkingLot 等待与唤醒](parking-lot.md) | `src-bthread/parking_lot.h` | 空闲 worker 休眠、通知状态与防止错过唤醒 |
| [VersionedRefWithId 版本化引用与回收竞争](versioned-ref-with-id.md) | `inc/versioned_ref_with_id.h` | 版本/引用计数编码、两个回收者的 CAS 竞争、`ver1+1==ver2` 兜底回收 |

## 记录约定

每篇记录包含问题背景、源码位置、实现模型、关键竞争过程、容易误解的地方和自测问题。源码位置以函数名为主；并发时序示例用于解释指定场景，不代替完整的内存模型正确性证明。

学习路径见 [bthread 核心学习计划](../impl/bthread-learning-plan.md)。
