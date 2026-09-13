# WorkStealingQueue 的 push/pop/steal

> 记录日期：2026-09-25。
> 源码：[work_stealing_queue.h](../../src-bthread/work_stealing_queue.h)，命名空间 `fast`。
> 学习位置：bthread 学习计划阶段 ⑤；可以先读本篇理解队列，后续再联系调度调用者。

## 1. 要解决什么问题

一个 worker 的本地就绪任务需要高效入队、取出；其他 worker 空闲时可以从它的队列窃取任务。

`WorkStealingQueue<T>` 是固定容量的环形双端队列。bthread 中通常存放 `bthread_t`，调度器再通过 ID 查找 TaskMeta，并不是直接在队列中存函数或协程栈。

- owner 在 bottom 端 push/pop，呈现后进先出的取任务顺序。
- thief 在 top 端 steal，从较早入队的任务开始取。
- push 不允许与 push/pop 并发；pop 不允许与 push/pop 并发。
- steal 允许与 push、pop、其他 steal 并发。

这种约束由调用者保证，类内部没有检查当前线程是否为 owner。不是任意多生产者、多消费者队列。

## 2. 下标和区间模型

`_top` 和 `_bottom` 是原子逻辑下标，不是内存指针。初始化均为 1；忽略整数回绕，稳定状态下有效元素对应半开区间 `[top, bottom)`。

```
                  top                           bottom
                   ↓                              ↓
逻辑下标：          10          11          12       13
元素：              A           B           C       下次 push 的位置
                   ↑                       ↑
                 steal                    pop
```

实际数组下标为 `index & (capacity - 1)`，所以容量要求是非零的 2 次幂。逻辑下标增长时，物理槽位循环复用。

pop 竞争过程中允许短暂出现 `top > bottom`；这是协议中的中间状态，不等于队列已损坏。`volatile_size()` 使用两次独立 relaxed 读取，适合估计长度，不能当成一致快照或后续操作的成功保证。

## 3. push：先写元素，再发布 bottom

对应源码 `WorkStealingQueue::push`：

1. relaxed 读取 bottom，acquire 读取 top。
2. 若 `b >= t + capacity`，队列满，返回 false。
3. 写 `_buffer[b & (capacity - 1)] = x`。
4. release 写 `_bottom = b + 1`，向 thief 发布新元素。

owner 独占 bottom 的写入，所以这里不需要 CAS。但 bottom 仍由 thief 并发读取，因此不能改为普通变量。

**顺序不能颠倒**：如果先发布 bottom 再写元素，thief 可能看到“有任务”却读到尚未写好的槽位。release 发布与 thief 对 bottom 的 acquire 读取用于建立必要的可见性关系。

读取 top 不代表获取一个永久有效的最新值；如果看到较旧 top，可能保守地报告满。调用者要根据返回值处理，而不是假定读到一次容量信息就能保证随后入队。

## 4. pop：先缩小 bottom，再按剩余区间判断

对应源码 `WorkStealingQueue::pop`：

1. 读取 b、t；如果 `t >= b`，快速判空并返回 false。
2. 计算 `newb = b - 1`，relaxed 写 bottom = newb，先把尾部槽位从公开区间中收回。
3. 执行 seq_cst fence，再次读取 top。
4. 按新的 t 与 newb 的关系处理。

| 条件 | 含义 | 行为 |
|------|------|------|
| `t > newb` | 尾部候选元素已无法由 owner 取得；并发窃取已推进 top | 恢复 bottom = b，返回 false |
| `t < newb` | top 与尾部候选元素是不同位置 | 读取尾部元素，返回 true，保留 bottom = newb |
| `t == newb` | owner 与 thief 可能争同一个最后元素 | 用 top 上的 CAS 仲裁；恢复 bottom = b，返回 CAS 结果 |

第二次 top 读取不是“无条件读到现实时间上的最新值”。正确性需要结合 fence、原子操作顺序以及 steal 的复查/CAS 协议理解，不能只归功于“又读了一次”。

### 为什么 `t < newb` 不需要 owner 做 CAS

缩小后的 bottom 不再把尾部候选槽位暴露为可窃取范围，steal 也会复查 bottom。top 尚在更前面时，owner 可以取尾部，thief 竞争头部。

即使仍有 thief 在运行，也不代表它们与 owner 正在竞争同一个槽位。这依赖双方完整的边界和内存序协议，不能只用“pop 不会与 pop 并发”来证明。

### 最后一个元素不是 owner 主动失败

实际代码：

```cpp
const bool popped = _top.compare_exchange_strong(
    t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
_bottom.store(b, std::memory_order_relaxed);
return popped;
```

owner 和 thief 都尝试把 top 从 t 改为 t + 1，只有成功者拥有该元素。

下面假设 owner 第二次读取时看到 `t == newb`，而 thief 已观察到允许它竞争的队列状态：

| 步骤 | owner 赢 | thief 赢 |
|------|----------|----------|
| 初始只有元素 A | top=10，bottom=11 | top=10，bottom=11 |
| owner 缩小 bottom | bottom=10 | bottom=10 |
| owner 复查 top | 读到 10 | 读到 10 |
| CAS 仲裁 | owner 的 10→11 成功 | thief 的 10→11 先成功，owner CAS 失败 |
| owner 恢复 bottom=11 | top=bottom=11，返回 true | top=bottom=11，返回 false |

**恢复 bottom 不等于恢复元素。** owner CAS 成功时 top 已经前进，恢复 bottom 后两者相等，队列保持空状态。如果 CAS 失败，元素已归竞争者，owner 同样只恢复空队列的边界。

如果 thief 在 owner 第二次读 top 之前就已推进 top，owner 会进入前面的 `t > newb` 分支，不再执行最后元素 CAS。

## 5. steal：CAS 失败后可能重试

对应源码 `WorkStealingQueue::steal`：

1. acquire 读取 top、bottom，若 `t >= b` 则返回 false。
2. 在循环中执行 seq_cst fence，并重新 acquire 读取 bottom。
3. 再次判空，避免仅凭先前看到的 bottom 继续取元素。
4. 先读取 `_buffer[t & (capacity - 1)]`。
5. CAS 尝试把 top 从 t 改为 t + 1；成功则返回 true。
6. CAS 失败时，expected 参数 t 会被更新为比较时观察到的 top，循环重新复查 bottom 并读取候选元素。

所以该队列没有 mutex 或条件变量等待，但 **steal 有 CAS 重试循环**。无锁不代表没有重试、不消耗 CPU，也不保证每次调用在固定步数内成功；不能把它直接等同于 wait-free。

false 也不总意味着“存在一个调用者可以依赖的持续空状态”：源码明确允许为了性能出现 false negative，而且返回后别的线程还可以立即 push。

pop 的最后元素竞争和 steal 都可能在最终失败前写过 `*val`。调用者只能在返回 true 时使用输出值作为成功取到的任务。

## 6. 与调度器的关系

- [task_group_inl.h](../../src-bthread/task_group_inl.h) 的 `push_rq` 向本地队列提交任务。
- [task_group.cpp](../../src-bthread/task_group.cpp) 的 `sched` / `ending_sched` 包含本地取任务路径；注意不同条件编译分支可能使用 pop 或 steal。
- [task_control.cpp](../../src-bthread/task_control.cpp) 的 `steal_task` 从其他 group 获取任务。
- 外部线程提交还涉及 [remote_task_queue.h](../../src-bthread/remote_task_queue.h)，它不是随意从外部调用本地队列 push。

队列操作本身不做阻塞等待，不等于整个调度过程不会等待：例如 `push_rq` 遇到队列满时会重试并调用 usleep；空闲 worker 则可能进入 ParkingLot 等待。

`_top` 的缓存行对齐也有性能目的：尽量分离 thief 频繁修改的 top 与 owner 修改的 bottom，减少缓存行相互干扰。仅保留原子类型而改变数据布局，也可能改变性能。

## 7. 对本次理解的修正

原理解中正确的部分：owner 串行执行 push/pop、bottom 只有 owner 修改、pop 与 steal 并发、pop 缩小 bottom 后要复查 top、最后一个元素需要特殊处理。

需要修正：

1. “无锁保证没有自旋等耗时流程” → steal 存在 CAS 重试，无锁不等于固定延迟。
2. “增加 bottom 并新增元素” → 严格按先写元素、后 release 发布 bottom 的顺序。
3. “top > bottom 导致队列功能异常” → pop 中可以是合法的临时状态，由协议恢复。
4. “读当前最新 top” → 再次读取配合 fence 和对端原子协议，不是一般意义的实时最新快照。
5. “最后一个元素 owner 还原 bottom、主动失败” → owner 先参与 CAS，返回胜负；恢复 bottom 只是归一化边界。

## 8. 自测问题

- 为什么 bottom 只有一个写者，仍然需要原子类型和 release 发布？
- 为什么 owner 在最后一个元素上也要修改通常由 thief 推进的 top？
- owner 最后元素 CAS 成功后，恢复 bottom 为什么不会让元素重新出现？
- steal CAS 失败后，为什么要重新读取 bottom，而不只重试 CAS？
- 为什么返回 false 时不能使用 `*val`？为什么 `volatile_size() > 0` 不保证随后 pop 成功？

本篇按当前源码解释关键路径和竞争场景，未对弱内存模型、环形槽位长期复用或整数回绕给出完整形式证明，也没有通过实验宣称延迟上界。
