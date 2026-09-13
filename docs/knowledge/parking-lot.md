# ParkingLot：空闲 worker 的等待与唤醒

> 源码：[parking_lot.h](../../src-bthread/parking_lot.h)、[task_group.cpp](../../src-bthread/task_group.cpp) 的 `wait_task`、[task_control.cpp](../../src-bthread/task_control.cpp) 的 `signal_task`。

## 用途

worker 找不到任务时，如果持续扫描队列和窃取，就会空耗 CPU。ParkingLot 通过 futex 让空闲 worker pthread 等待，并在任务到来或调度器停止时唤醒它。

| 结构 | 职责 |
|------|------|
| WorkStealingQueue / RemoteTaskQueue | 保存可执行任务 |
| ParkingLot | 等待、唤醒空闲 worker pthread |
| butex | 让任务或线程等待某个同步条件 |

ParkingLot 不存放任务，也没有用户态 worker 链表；futex 等待线程由内核管理。当前每个 tag 默认有 4 个 ParkingLot，worker 被分配到其中一个，分散等待和通知的竞争；一个 ParkingLot 可由多个 worker 共用。

## 状态与接口

核心原子变量 `_pending_signal` 的最低位表示停止，其余位随通知变化。它用于检测状态变化，不是就绪任务数量或队列长度。

| 接口 | 行为 |
|------|------|
| `get_state()` | 读取状态，供之后等待时比较 |
| `wait(expected)` | 状态不匹配则返回；匹配时调用 futex 等待 |
| `signal(n)` | 状态增加 `n << 1`，尝试唤醒至多 n 个线程 |
| `stop()` | 设置停止位，并发出唤醒通知 |

`_waiter_num` 配合可选开关统计等待线程；开启后，signal 观察到无人等待时可跳过 futex_wake，但仍更新状态。worker 醒来后需要重新检查停止状态或寻找任务，唤醒本身不保证取得任务。

## 为什么需要状态比较

单纯“没任务就睡、有任务就唤醒”可能错过通知：提交线程在 worker 检查完队列、尚未睡眠时发出通知，worker 随后才睡下。

配合调度器的正确调用顺序，状态机制处理这个窗口：

```text
worker A：记录状态 S → 查找任务，未找到
线程 B：  发布任务 → signal 将状态改为 S'
worker A：wait(S) 发现状态不匹配，返回并重新找任务
```

如果通知发生在 wait 的用户态检查之后，内核 futex 的“比较期望值并进入等待”机制继续处理这个竞争。不能只靠普通变量的检查后睡眠实现同样的协议。

当前默认路径在 `TaskGroup::steal_task` 中保存 `_last_pl_state`，在后续 `wait_task` 中使用；另一个条件编译分支在 `wait_task` 内记录状态、查任务、等待。阅读时要把状态记录位置与任务检查顺序连起来看。

## 自测

为什么 worker 检查队列后不能直接睡眠？为什么 ParkingLot 的通知状态不等于任务数量？为什么被唤醒后仍要重新寻找任务？
