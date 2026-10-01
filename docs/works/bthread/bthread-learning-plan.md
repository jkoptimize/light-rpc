# bthread 核心学习计划（初学者向）

> 面向有 C/C++ 基础、刚接触协程 / M:N 调度的读者。以本项目 `src-bthread/` 的移植代码为主要学习对象，按「接口 → 一条完整路径 → 栈与切换 → 调度 → 同步」逐步深入。
>
> 更新：2026-09-25。源码定位使用文件名和函数名，避免移植过程中行号变化影响阅读。原版 `/home/syt/Desktop/brpc/brpc/src/bthread/` 只用于核对移植差异，不要求切换到原版学习或运行。
>
> 当前进度：已读完 `src-bthread/bthread.h` 的声明，处于阶段 ① 收尾，可以进入阶段 ②。阅读声明不等于掌握所有 API；先跟踪 `bthread_start_background`，其余接口按需要回看。

## 0. 一句话理解 bthread

bthread 是 M:N 用户态任务调度库：多个 bthread 任务由一组 worker pthread 执行。普通栈 bthread 保存自己的执行现场，通过用户态换栈和恢复寄存器暂停、继续执行。

**必须先区分三件事**：

- **任务挂起**：普通栈 bthread 调用支持协程等待的接口，调度器可让 worker 去执行其他任务。
- **线程阻塞**：普通阻塞系统调用、pthread 同步操作仍可能阻塞当前 worker；bthread 不会自动改造任意阻塞操作。
- **worker 休眠**：没有可运行任务时，worker 可以通过 `ParkingLot` / futex 主动等待工作。

`BTHREAD_STACKTYPE_PTHREAD` 是特殊执行模式，使用 worker 的线程栈，其等待行为与普通栈 bthread 不同。`butex_wait` 和 `bthread_join` 的行为也要结合调用者类型理解。

**学习主线**：先回答「一个任务怎么跑完」，再回答「怎么切换」「怎么等待」「为什么不会发生竞态」。不预设协程切换的固定性能倍数。

---

## 1. 前置知识（按需补齐）

| 概念 | 第一轮需要掌握 | 后续何时深入 |
|------|----------------|--------------|
| pthread 与 TLS | 线程共享进程地址空间；线程栈、`pthread_create/join`、`thread_local` 的归属 | 阶段 ④ 学任务迁移，阶段 ⑨ 学 bthread TLS |
| 栈与函数调用 | 局部变量、返回地址、栈指针，函数返回后局部对象生命周期 | 阶段 ③ 再读 ABI 和寄存器 |
| 原子操作 | load/store、exchange、CAS；原子性和可见性不是同一概念 | 阶段 ⑥ 学发布关系，阶段 ⑨ 深入队列内存序 |
| mutex / cond | 临界区；在锁内用 while 循环检查条件变量谓词 | 阶段 ⑦ 对照 bthread 实现 |
| Linux futex | 期望值匹配才等待；wake 唤醒线程；等待可能失败或被打断 | 阶段 ⑤、⑥ 结合两种用途阅读 |

不要求先掌握全部六种 memory order、所有无锁算法或全部汇编指令。遇到一个具体同步关系，再补它需要的知识。

**先定位**：`types.h` 中的 `bthread_t`、`bthread_attr_t`、`BTHREAD_ATTR_NORMAL`、`BTHREAD_STACKTYPE_PTHREAD`。多 tag、追踪属性暂时只知道存在。

---

## 2. 学习路径总览

```
① 接口与心智模型（当前已读完 bthread.h 声明）
      │
② 从 start_background 跟踪一个任务的完整执行路径 ← 下一步
      │
③ TaskMeta、栈、上下文切换（最后才深入汇编）
      │
④ sched / sched_to / ending_sched、yield、remained 回调
      │
⑤ 本地队列、远程队列、work stealing、worker ParkingLot
      │
⑥ butex：先学无超时等待，再分析防丢失唤醒
      │
⑦ mutex / condition_variable / once
      │
⑧ usleep / timer / join：补齐超时、完成与回收
      │
⑨ TLS、id、多 tag、内存序与性能细节
```

每一阶段分两遍：第一遍跟踪正常路径，第二遍再看并发竞争和异常路径。完成标准以能解释代码为准，不设必须在几小时内掌握的要求。

---

## 3. 分阶段详解

### 阶段 ① 接口与心智模型（当前阶段收尾）

**文件**：`src-bthread/bthread.h`、`src-bthread/types.h`。

**要点**：

- 区分 `bthread_t`（任务身份）与 `pthread_self()` / Linux TID（执行任务的线程身份）。
- 第一轮只关注 `bthread_start_background`、`bthread_start_urgent`、`bthread_join`、`bthread_yield`、`bthread_usleep`。
- background 创建后任务会被排队，但其他 worker 可能在创建调用返回前执行它；不要把 background 理解成严格的执行时序保证。
- urgent 的行为取决于调用者上下文；普通 pthread 调用它也会走非 worker 提交路径。
- 同一个 bthread 恢复时可能运行在另一 worker 上；普通 TLS 不等于 bthread local storage。
- 当前头文件保留了部分尚未移植的接口声明，不能据此认定实现和链接已经齐全。

**验证**：能说清任务和 worker 的区别，以及为什么 `bthread_join` 是否阻塞 OS 线程要看调用上下文。

**当前行动**：补看以上 `types.h` 类型，然后进入阶段 ②；不必重新通读全部声明，也不必先读汇编。

---

### 阶段 ② 从接口跟踪完整路径（下一步）

**文件**：`bthread.cpp`、`task_control.h/.cpp`、`task_group.h/.cpp`、`task_group_inl.h`。

**第一条路径的假设**：普通 pthread 调用 `bthread_start_background`，属性为默认值，不启用多 tag、priority、NOSIGNAL，不考虑内存分配失败。

**阅读顺序**：

1. `bthread.cpp::bthread_start_background`：看 `tls_task_group` 如何区分调用环境。
2. `start_from_non_worker`：先看到获取 `TaskControl`、选择 group、调用 `start_background<true>`。
3. `get_or_new_task_control` → `TaskControl::init` → `worker_thread`：理解 worker 的惰性初始化。线程一旦创建就可并行执行，不是提交线程的同步调用链。
4. `TaskGroup::start_background<true>`：获取 TaskMeta、设置 fn/arg/属性、生成 tid、进入远程队列。这里不立即分配任务栈。
5. `ready_to_run_remote` → `TaskControl::signal_task`：先掌握「发布任务并通知 worker」，暂不钻研队列算法。
6. worker 的 `run_main_task` → `wait_task` → `sched_to`：看到如何拿到任务；首次获取栈的逻辑在 `task_group_inl.h` 的重载中。
7. `TaskGroup::task_runner`：找到执行 `m->fn(m->arg)` 的位置，以及完成后的版本更新、唤醒 join 等待者、安排资源回收。
8. `bthread_join` → `TaskGroup::join`：先把 butex 当成等待机制，阶段 ⑥、⑧ 再解释实现。

```
提交线程：start_background → TaskControl → 选 group → 初始化 TaskMeta → 远程入队/通知
                                                         │
worker：wait_task → sched_to → 获取栈/恢复上下文 → task_runner → fn(arg)
                                                                  │
                         完成 → 版本更新/唤醒 join → 切换并回收资源
```

**第二条路径**：已有普通 bthread 调用 `bthread_start_background`，跟到 `start_background<false>`，比较本地与远程入队；然后再比较 urgent / foreground。

**暂时跳过**：汇编指令、CAS 证明、多 tag、profiling、tracer、异常分支和池内部实现。

**验证**：画出提交线程与 worker 两条执行线，指出用户函数实际在哪里调用；解释为什么函数写在同一个 cpp 中不代表由同一个线程顺序执行。

---

### 阶段 ③ 任务元数据、栈与上下文

**文件**：`task_meta.h` → `stack.h` → `stack_inl.h` → `stack.cpp` → `context.h` → `context.cpp` 的 x86_64 分支。

**要点**：

- TaskMeta 保存 fn、arg、属性、stack、local_storage、version_butex 等；第一轮只追踪这些字段的写入与读取。
- `make_tid(version, slot)` 把版本号放在高 32 位、资源槽位放在低 32 位。ResourceId 提供槽位，版本由任务协议管理。
- `StackStorage` 管理内存，`ContextualStack` 还保存上下文；普通任务栈通过对象池复用。
- small/normal/large 默认是 32 KiB / 1 MiB / 8 MiB，可配置；PTHREAD 模式是例外。
- guard page 启用时，访问保护页会触发错误；不要理解为能检测所有形式的栈越界。
- `StackFactory::Wrapper` 调用 `bthread_make_fcontext`，入口是 `task_runner`，再由 runner 调用用户函数。
- `jump_stack` 调用 `bthread_jump_fcontext`。先理解保存/恢复和返回位置，再读 ABI 要求的寄存器与栈布局。

**验证**：画出两个任务的栈；指出第一次运行与挂起后恢复的区别，解释为何返回到 `jump_stack` 后面的代码可能已经是另一个时刻、另一个 worker。

---

### 阶段 ④ 任务切换、yield 与切换后的回调

**文件**：`task_group.cpp`、`task_group_inl.h`、`bthread.cpp`。

**阅读顺序**：`bthread_yield` → `TaskGroup::yield` → `sched` → `sched_to`；随后阅读 `ending_sched`、`task_runner`、`set_remained` 的调用点。

**要点**：

- 调度可以从任务 A 直接切到任务 B，不必每次回到 worker 主栈。
- 暂停后还要恢复的任务与已经结束的任务，栈和 TaskMeta 的处理不同。
- `set_remained` 安排切换之后执行的操作；它与任务重新入队、等待者注册、旧资源回收相关。
- 不能在当前任务仍使用自己的栈时提前归还它，也不能让同一个任务同时在两个 worker 上执行。
- 切换后重新读取 `tls_task_group`；TaskGroup 属于 worker，任务可能迁移。
- bthread 以协作方式让出执行机会，长时间计算且不让出的代码会占用 worker；yield 也不是公平性保证。

**验证**：画出 A yield 到 B 再恢复的时间线，标出 remained 回调在哪个执行现场运行，解释为什么要放在切换之后。

---

### 阶段 ⑤ 就绪队列、工作窃取与 worker 休眠

**文件**：`work_stealing_queue.h`、`remote_task_queue.h`、`task_group.h/.cpp`、`task_control.h/.cpp`、`parking_lot.h`、`sys_futex.h`。

**要点**：

- 本地队列：owner push/pop 与其他 worker steal 的访问约束。
- 远程队列：外部线程提交任务，保留它的锁与队列协议，不能因名称像无锁队列就假设所有操作都无锁。
- 第一轮理解 push/pop/steal 谁调用；第二轮才分析「最后一个元素被 owner 与 thief 同时争抢」。
- `wait_task`、`steal_task`、`signal_task` 串起找任务、找不到任务和发布任务。
- **ParkingLot 等待的是 worker；butex 等待的是某个同步条件。** 两者不是同一张等待队列。
- 观察 worker 在检查任务与 futex 等待之间怎样利用状态变化避免漏掉通知。

**验证**：画出本地提交、外部提交、其他 worker 窃取三条路径；解释有任务时为何仍需考虑唤醒休眠 worker。

---

### 阶段 ⑥ butex：先学习无超时等待

**文件**：`butex.h/.cpp`、`sys_futex.h`，回看 `task_group.cpp::sched_to`。

**阅读顺序**：`Butex` / 两种 waiter → `butex_wait` → `butex_wait_from_pthread` → `wait_for_butex` → `butex_wake`。

**要点**：

- `Butex` 内部含等待者链表和保护锁，调用者拿到的是其中的值地址。
- 普通 pthread 与 PTHREAD 栈任务走线程等待；普通栈 bthread 通过调度挂起。
- 先看 `abstime == nullptr`，暂不追踪定时器与 interruption 的所有分支。
- bthread 路径设置 remained 回调，在切换后持 `waiter_lock` 再检查期望值并决定是否入队。
- 防丢失唤醒依赖检查值、锁、入队与调度协议，不能只归因于 futex 原子性。
- wake 让任务重新变成可运行；它不是调用者业务条件一定成立的证明。

**验证**：分别画「wake 先发生」「wait 入队后 wake」的时间线，解释为何不会永久睡下；指出栈上 waiter 在什么期间必须保持有效。

---

### 阶段 ⑦ mutex、condition_variable 与 once

**文件**：`mutex.h/.cpp`、`condition_variable.h/.cpp`、`bthread_once.cpp`。

**要点**：

- mutex 先看 `MutexInternal`、状态常量，再串起 init、trylock、contended lock、unlock；不能只看加锁函数。
- 区分 acquire/release 的可见性与 butex 的排队唤醒职责。
- 被唤醒后需要重新竞争锁；自旋只是有限的优化，不代表一直占着 worker 忙等。
- cond 跟踪「记录 seq → 解锁 → butex 等待 → 重新加锁」。调用者在锁内用 while 检查谓词。
- signal 修改序号并唤醒；broadcast 还涉及 requeue，放在第二轮理解。
- once 是同一个控制对象对应的一次初始化，可作为简短的 butex 应用案例，不是每个 bthread 各执行一次。
- 当前 mutex 尚未完成移植：第一轮按上述函数定位核心逻辑，跳过 profiler、hook 和调试块；运行实验等本项目编译接通后进行。

**验证**：解释丢失唤醒、虚假唤醒、锁竞争的区别；画出两个任务竞争 mutex 的完整过程。

---

### 阶段 ⑧ 定时、join 与资源生命周期

**文件**：`timer_thread.h/.cpp`、`task_group.cpp`、`butex.cpp`、`task_group_inl.h`。

**阅读顺序**：`bthread_usleep` → `TaskGroup::usleep` → `_add_sleep_event` / 定时回调 → TimerThread schedule/unschedule/run → butex 超时 → join 完成协议。

**要点**：

- TimerThread 是独立线程；回调把到期任务重新投递给 worker。
- 先理解 bucket 与最小堆的分工，再分析取消与回调正在执行时的返回值。
- butex 的 timeout、wake、interrupt 可能竞争；需要保证 waiter 不被重复处理，回调结束前栈上对象不失效。
- `TaskGroup::join` 等待版本变化；`task_runner` 完成时更新版本并唤醒等待者。
- 把任务逻辑完成、唤醒 join、切走、回收 TaskMeta/栈区分开，不能假设是同一步。
- `stop/interrupt` 按接口语义理解，不把它当作任意时刻强杀任务。

**验证**：画出 sleep 正常到期、超时与 wake 竞争、旧 tid 对应槽位被复用三种场景。

---

### 阶段 ⑨ 进阶（按需要选读）

| 内容 | 文件 | 重点问题 |
|------|------|----------|
| bthread TLS | `key.cpp`、`task_group.cpp` | 为什么任务迁移后仍能看到自己的数据，何时析构？ |
| pthread TLS 基础 | `butil/thread_local*`、`butil/thread_key*` | 为什么 thread_atexit 与普通 thread_local 不能直接互换？ |
| 池内部结构 | `butil/resource_pool*`、`butil/object_pool*` | TLS 缓存如何减少共享锁与频繁分配？ |
| 异步 ID | `id.h/.cpp`、`list_of_abafree_id.h` | 为什么 bthread_id 与 bthread_t 是两套协议？ |
| tag / priority / NOSIGNAL | `bthread.cpp`、`task_control*`、`task_group*` | 哪些分组和批量提交策略影响任务投递？ |
| 原子与性能 | 队列、butex、池的热点 | 原来的内存序、缓存行布局、竞争分散是否保留？ |

**验证**：选一个功能，用「接口行为 → 数据结构 → 并发协议 → 性能代价」四步解释。

---

## 4. 实践建议

1. **学习以移植代码为主**：所有上述路径都从 `src-bthread/` 查找。遇到缺失定义时记录为移植问题，必要时核对原版，不把缺失当作算法设计。
2. **当前先做纸面跟踪**：本项目 `fast_bthread` 目前只编译 butil 源文件，核心尚未完成编译接入。现在可以读路径、画时间线，不能把现有构建成功当作协程实验已可运行。
3. **实验只在本项目接通后运行**：先 start/join，再 yield/usleep，再 mutex/cond，最后做超时竞争和多 worker 压力实验。无需改用原版运行来满足学习进度。
4. **同时观察任务和线程身份**：记录 `bthread_self()` 与 Linux TID；只打印 bthread ID 无法知道用了几个 worker。迁移允许发生，但不能要求每次 yield 都迁移。
5. **实验需要明确同步**：用谓词、原子状态或同步原语建立顺序，不能用一次 sleep 假定另一个任务已入队。计时结果受调度影响，不把固定百分比误差当作通用正确性条件。
6. **每阶段留下一个成果**：函数调用链、字段生命周期表、两线程竞争时间线或可解释的实验结果；读完文件不等于完成阶段。
7. **区分监控与基础语义**：profiling、tracer、监控导出可以跳过；线程退出回调、错误码、配置合法性、内存序和资源回收不能按维测代码忽略。
8. **基础设施优先复用**：IOBuf 相关设施使用项目 `src-common/` 和 `inc/fast_iobuf.h`；学习及后续移植不重复搬运。

当前移植检查详见 [bthread 移植偏差检查（mutex 之外）](bthread-port-review.md)。

---

## 5. 推荐阅读顺序（浓缩版）

```
bthread.h（已读）+ types.h（补关键类型）
→ bthread.cpp::bthread_start_background
→ start_from_non_worker / get_or_new_task_control
→ TaskControl::init / worker_thread
→ TaskGroup::start_background<true> / ready_to_run_remote
→ run_main_task / wait_task / sched_to / task_runner
→ TaskMeta / stack_inl.h / context（逐步打开黑盒）
→ yield / sched / ending_sched / remained
→ 本地与远程队列 / work stealing / ParkingLot
→ butex 无超时等待 / wake
→ mutex lock+unlock / cond / once
→ usleep / timer / butex 超时 / join 回收
→ TLS / id / 多 tag / 内存序
```

**你现在的下一步**：打开 `src-bthread/bthread.cpp` 的 `bthread_start_background`，按阶段 ② 的默认属性、普通 pthread 调用场景追踪。先找到 `m->fn(m->arg)`，再回头拆解这条路径里的组件。
