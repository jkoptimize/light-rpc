# RDMA endpoint 并发问题与修复计划

> 创建日期：2026-09-27；更新日期：2026-09-28。
> 状态：用户已要求按编号、每次一个问题实施。R01、R02 已实现，验证与边界见第 7 节；R03～R08、O01 尚未实施。
> 分析对象：当前工作区的 `src-endpoint/fast_rdma_endpoint.cc` 及相关头文件、事件分发与 channel/server 生命周期。
> 证据范围：静态源码审查、并发时序推演、R01 纯逻辑及 R02 Linux fd 回归；尚未通过 RDMA 实机验证，不代表已完成全部线程安全审计。

## 1. 结论与范围

当前存在可由代码推导出的并发正确性缺陷，可能导致同一 endpoint 被多个线程处理、请求丢失、永久等待及访问已释放内存。优先恢复正确性，再评估调度和锁竞争开销。

当前 `EventDispatcher` 只有一个 epoll 线程，回调串行执行；回调派生的 detached pthread、调用 `StartWrite` 的业务线程以及销毁线程可以并发。单个 epoll 线程不能保证 endpoint 的所有操作串行。

| 对象 | 用途 | 需要维持的约束 |
|---|---|---|
| `tcp_fd_` | 建连、握手 | 单个握手执行者，关闭与在途访问协调 |
| `comp_channel_->fd` | 获取 CQ 事件通知 | 同一 endpoint 只有一个有效 CQ 处理者 |
| 四个 CQ、接收缓冲区、消息解析器 | 获取完成结果并处理消息 | CQ 处理者串行维护相关状态 |
| `_write_head` | 汇集发送请求 | 多个生产者安全提交，单个写执行者消费 |
| QP、CQ、MR、IOBuf | 承载在途工作 | CPU 访问及硬件访问结束后才能回收 |

竞争问题不仅涉及 fd 上的系统调用，也涉及对应对象的缓冲区、索引、计数和生命周期。底层 verbs 对并发调用的支持不能代替上层对象同步。

### 实施原则

- 2026-09-28 用户已授权按问题编号逐项修复，每次只处理一个问题；实现与验证结果逐项记录。
- 平台限定 Linux，复用 `src-common` 已有 IOBuf 等基础设施。
- brpc 已有的并发算法和必要依赖应成套移植，保留原有语义与性能特征；不能只复制核心原子操作、删去配套协议。
- 标准库替换必须说明语义与性能等价性；本项目双 QP、四 CQ 等扩展需单独论证。
- 不给整个 endpoint 增加覆盖所有读写操作的大锁。注册管理等低频路径可用锁，数据处理保留原有单消费者设计。
- 涉及 `ibv_*` 的实现必须核对 verbs 规范及对应接口文档；具体 QP 停止、完成回收和 MR 释放顺序需在实施前形成有依据的步骤。
- 按 `CLAUDE.md`：纯逻辑可写单元测试，调用 RDMA 接口的场景采用实机集成验证；测试专用生产接口须注明 `// For unit tests only`。

## 2. 问题清单

P0：可能破坏内存安全、并发所有权或基本进展。P1：特定时序下遗漏工作或无法正常退出。优先级不代表发生概率。

| 编号 | 优先级 | 问题 | 主要后果 |
|---|---|---|---|
| R01 | P0 | `MoreReadEvents` 的 CAS 返回值缺少取反，错误退出也未闭合 | 多个 PollCq 并发，或后续事件无人处理 |
| R02 | P0 | completion channel 未设置非阻塞 | 获取完现有通知后阻塞，无法进入 CQ 处理 |
| R03 | P0 | 写队列缺失节点连接协议，失败清理不完整 | 空指针访问、请求丢失、悬空队头 |
| R04 | P0 | 先释放资源再等待线程；回调缺少生命周期保护 | 释放后使用、自等待死锁 |
| R05 | P0 | 多线程无锁修改 `_fd_map` | 容器数据竞争、注册上下文回收不完整 |
| R06 | P1 | 四个 CQ 开启通知后只复查最后一个 | 已到达 CQE 被遗留，等待后续事件才处理 |
| R07 | P1 | 批量 ACK 尾数未在销毁前处理 | `ibv_destroy_cq` 可能一直等待 |
| R08 | P1 | 条件变量的条件更新未与等待建立完整同步 | 可写状态已恢复，发送线程仍永久等待 |
| O01 | 优化项 | 每次新一轮处理创建 detached pthread | 线程创建、调度和缓存开销，尚无量化结论 |

### R01：读事件所有权协议错误

状态：2026-09-28 已修复事件交接和已识别错误返回的停止处理，纯逻辑验证已完成；统一资源回收仍属于 R04，详见第 7 节。

位置：`OnCompChannelEvent`、`PollCq`、`MoreReadEvents`，发现问题时约第 1064、1078、1187 行。

修复前代码返回 CAS 本身的结果，而调用方使用 `if (!MoreReadEvents(&progress)) break`。brpc `Socket::MoreReadEvents` 返回 CAS 结果的取反。

| 时序 | 修复前行为 | 正确语义 |
|---|---|---|
| `_nevent == progress`，CAS 清零成功 | 返回 true，旧处理者继续；新事件可启动另一处理者 | 释放处理权，旧处理者结束共享处理 |
| 新事件使 `_nevent != progress`，CAS 失败 | 返回 false，旧处理者退出，计数仍非零 | CAS 更新 progress，原处理者继续处理 |

风险涉及 `read_buf_`、消息解析器、收发索引、事件 ACK 计数等共享状态。部分错误路径直接返回，还可能遗留正数 `_nevent`。

方案：恢复原版事件计数、内存序及交接协议；逐条检查正常退出、停止、读取事件失败、轮询失败和任务启动失败。失败不能靠无条件清零掩盖，必须进入统一失败流程，避免同时释放处理权和继续访问共享状态。

注意：R02 可能先阻塞执行并掩盖 R01；修复 R02 后仍必须验证 R01 的交接行为。

### R02：非阻塞前提缺失

状态：2026-09-28 已补齐非阻塞设置、事件获取的 EINTR 处理和初始化失败回滚；编译及普通 Linux fd 测试通过，RDMA 实机待验证。

位置：`AllocateResources`、`GetAndAckEvents`，发现问题时约第 378、924 行。

修复前 completion channel 创建后直接注册 `EPOLLIN | EPOLLET`，但未设置 `O_NONBLOCK`。获取事件的循环却假定最终返回 `EAGAIN`。默认阻塞行为下，现有通知取完后会等待新通知，且此时尚未进入后面的 CQ 轮询和重新开启通知流程。

方案：注册前以 `F_GETFL` 获取原 flags，再设置 `flags | O_NONBLOCK`，检查所有返回值；`EINTR` 重试，`EAGAIN` 结束本轮获取，其他错误进入统一失败处理。初始化失败必须回滚已创建的资源和事件注册。

依据：libibverbs 的 `ibv_get_cq_event` 文档及非阻塞示例，见参考资料。

### R03：发送链表发布与回收协议不完整

位置：`WriteRequest`、`StartWrite`、`IsWriteComplete`、`KeepWrite`，当前约第 737、799、835 行。

可触发时序：

```text
A 持有旧队头 a，负责实际写出
B exchange 发布 b，但尚未执行 b.next = a 就被切走
A 发现新队头 b，开始反转链表
A 读到 b.next == nullptr，继续遍历时可能解引用空指针
```

brpc 使用 `UNCONNECTED` 标识尚未连接完成的节点，并在消费时处理该状态；当前实现没有这个协议。原子发布队头不能保证发布后的 `next` 写入已经完成。

错误路径也存在缺口：`StartWrite` 直接清空 `_write_head` 可覆盖并发提交；`KeepWrite` 仅释放当前可见链表，未完整处理原子队头与并发生产者。

方案：对照 brpc 成套恢复节点初始化、发布、连接、消费等待、反转、失败排空及回收协议，核对原子访问与必要依赖。不能仅增加一个哨兵值或一次 yield。明确失败期间是否接受请求，每个已接受请求必须完成或得到失败结果，且只回收一次。

原版允许消费者等待尚未连接的节点；不能将整个写执行流程描述为 wait-free。

### R04：生命周期和退出顺序不安全

位置：析构函数、`DeallocateResources`、握手回调，以及 channel/server 的析构和事件注销路径。

已发现的具体问题：

1. 析构先 `DeallocateResources()`，再等待 `_running_threads`。处理者可能已经取得 CQ、QP 或缓冲区指针，随后继续使用已释放资源。
2. `EPOLL_CTL_DEL` 不会等待已取出的事件或已经运行的回调结束。保留 `EventContext` 也不能保护其中裸指针 `user_data` 指向的 endpoint。
3. 回调检查 `_stop` 与增加 `_running_threads` 之间存在窗口；该计数也未覆盖同步 `StartWrite` 等所有使用者。
4. 服务端握手失败在线程计数减一前 `delete ep`，析构等待包含当前线程的计数归零，形成自等待。
5. `FastChannel` 析构持有 `pending_mutex_` 时删除 endpoint；若退出中的消息回调也需要该锁，等待处理线程退出可能形成锁依赖死锁。

方案：引入明确的停止入口与安全回收边界。操作和回调在访问对象前必须取得有效生命周期引用；不能先解引用可能悬空的 endpoint，再增加引用。处理者可报告失败、请求停止，由安全的所有者完成最终回收，不能等待自身退出。

详细资源退出顺序见第 3 节。TCP 握手、连接失败、server 关闭、channel 关闭必须纳入同一审查，不能只保护 comp channel。

### R05：注册表与事件上下文回收

位置：`EventDispatcher::RegisterEvent`、`UnregisterEvent`、`RunEpollLoop`。

多个连接和握手线程可同时执行 `_fd_map[fd] = ctx`，当前没有容器同步。注销只执行 epoll DEL，上下文保留至 dispatcher 析构；fd 复用会覆盖 map 项，旧上下文可能无法回收。头文件关于注销即释放上下文的注释也与实现不一致。

方案：低频注册管理采用 mutex 或事件线程命令队列；将“注册失效”和“上下文可回收”分开。推荐在事件线程统一执行注销和批次回收，通过 eventfd 唤醒；事件线程自身发起注销时不得同步等待自己。

事件上下文需关联稳定的注册身份及 endpoint 生命周期引用，不能只凭整数 fd 判断归属。无论采用何种方案，都要覆盖 epoll 已取出事件、同批次残留事件和 fd 复用；不能用固定延时代替安全回收证明。

### R06：四 CQ 的通知与复查没有闭合

位置：`PollCq`，当前约第 1098～1116 行。

```text
检查 recv_cq 为空
recv_cq 在未开启通知期间产生 CQE
给四个 CQ 重新开启通知
只复查 data_send_cq，随后准备退出
recv_cq 的既有 CQE 没有被处理
```

通知是一次性的；重新开启通知不会为已有 CQE 补发事件。原有四 CQ 扩展必须建立完整的“开启通知后复查”流程。

方案：维护一轮覆盖全部四个 CQ 的复查状态。开启通知后从第一个 CQ 开始复查，只有全部满足退出条件后才能交还事件处理权；检查每次 `ibv_req_notify_cq` 的返回值。保留 solicited 通知的原有意图，并核对所有对应发送路径是否设置所需标志。

不得简单机械地重置 phase 而忽略 notified 的更新条件，避免产生永久重启扫描或遗漏某个 CQ 的新窗口。

### R07：CQ 事件 ACK 尾数遗漏

位置：`GetAndAckEvents`、`DeallocateResources`。

当前累计至少 128 个事件才批量 ACK，关闭时没有补齐不足阈值的剩余计数。成功获取的事件必须被 ACK，CQ 销毁会等待相关 ACK。

方案：保留运行时批量 ACK；事件处理者退出后统一处理已成功获取但尚未 ACK 的计数。关闭期间如继续获取事件，必须纳入同一记账。区分“已获取事件”和“channel 中尚未获取的通知”，不能猜测数量或重复 ACK。

### R08：可写等待可能丢唤醒

位置：`WaitForWritable`、`HandleCompletion`、停止时的 `send_cv_.notify_all()`。

```text
写线程：持有 send_mutex_，检查到不可写
完成线程：更新原子窗口为可写，调用 notify（未参与同一 mutex 协议）
写线程：进入 wait
没有后续通知，写线程持续等待
```

原子变量保证计数访问的原子性，但不能自动保证条件检查与进入等待之间不丢通知。

方案：优先对照 brpc 原有等待/唤醒协议及依赖，分析完整移植可行性。若当前阶段保留 condition_variable，必须让条件更新、等待登记和通知形成完整同步协议，并说明热路径加锁成本；不能直接认定它与原版等价。

同时核对窗口阈值：若已经可写但未达到通知阈值，必须证明仍存在可靠的后续唤醒来源。停止和失败必须唤醒全部等待者。

### O01：线程创建与调度开销

当前每轮取得处理权都会创建 pthread 并 detach，握手和后台发送也使用临时线程。可能增加创建、调度和缓存开销，但尚未测量，不能给出性能下降比例。

方案：完成正确性修复后单独评估复用执行线程；bthread 核心及依赖编译、验证完成后再考虑接入。会阻塞的握手或发送任务不能直接塞入数量有限的公共线程池，否则可能阻塞负责恢复进展的任务。

## 3. 整体方案

### 3.1 所有权约束

- 一个 endpoint 同时至多有一个有效 CQ 消费者和一个写消费者；两者可以并行，跨方向共享字段必须另有同步依据。
- 业务线程只按完整队列协议发布请求，不能绕过写所有权修改发送状态。
- 事件计数只负责合并事件和交接处理权，对象寿命由单独的引用/回收协议保证。
- 初始化成功后再发布可用对象；关闭开始后不再接纳新的有效操作。
- 对象处于停止过程中时，既有任务必须能观察退出条件并结束，不能继续派生无保护的新任务。

### 3.2 关闭与失败处理

建议建立 `初始化 → 运行 → 停止中 → 已关闭` 的状态模型；初始化失败也进入停止流程。具体字段和所有权类型在实施阶段对照原版确定。

1. 原子地进入停止状态，拒绝新提交；保证重复停止安全。
2. 使 TCP/CQ 事件注册失效，处理已分发回调，建立 dispatcher 与回收方之间的完成确认。
3. 唤醒发送等待，取消或结束握手等待；所有等待路径检查停止/失败，具备明确的退出方式。
4. 等待 CPU 侧回调、同步操作、握手、CQ 处理和写任务不再访问资源；避免持有这些任务需要的锁，也避免任务等待自身。
5. 按 verbs 规范完成 QP 停止和必要的完成/事件处理，确认硬件不会再访问待释放内存；确有需要的最终 drain 由唯一关闭执行者负责。
6. 补齐已获取 CQ 事件的 ACK，按依赖顺序销毁 QP/CQ/channel，安全释放 MR、缓冲区及 fd。
7. 完成未决请求的失败通知并释放最后的对象引用。通知的实际时机应保证调用方不会提前回收仍被访问的资源。

这是 CPU 生命周期与 RDMA 资源生命周期共同组成的流程。实现前必须明确第 5、6 步的具体 verbs 顺序及依据，不能只把现有 `DeallocateResources()` 整体移动到等待之后。

### 3.3 依赖闭合清单

| 修改对象 | 必须连带检查 |
|---|---|
| `MoreReadEvents` | OnCompChannelEvent、PollCq 全部退出、任务启动失败、内存序 |
| 写队列 | WriteRequest、发布、连接、反转、失败排空、对象池/分配回收、请求失败通知 |
| endpoint 关闭 | dispatcher、TCP 握手、channel/server 所有者、回调引用、同步提交、QP/CQ/MR/IOBuf |
| 可写等待 | 窗口更新、阈值策略、停止与失败通知、底层等待设施 |
| CQ 通知 | 四 CQ 扫描、solicited 标志、非阻塞 channel、ACK 记账和销毁 |

## 4. 分阶段实施计划

以下为最初的阶段划分。2026-09-28 起按用户要求改为每次处理一个问题编号；依赖检查与最终集成验收要求仍保留。R01、R02 已实施，其余问题待逐项处理。

| 阶段 | 工作项 | 交付与验收 |
|---|---|---|
| 0：基线与方案确认 | 固定项目工作区差异与 brpc 版本；梳理调用图、共享字段、必要依赖；确定引用/注销协议和 verbs 退出步骤 | 方案经确认；每项改动能追溯到问题编号；记录当前构建测试基线 |
| 1：事件所有权 | R01、R02、R06；恢复单消费者事件协议、非阻塞读取、四 CQ 复查与错误处理入口 | 纯逻辑交接测试通过；编译通过；逐条说明退出路径，不提前宣称资源关闭安全 |
| 2：发送与等待 | R03、R08；完整移植写队列协议，闭合失败排空，修复等待登记与唤醒 | 多生产者确定性时序测试、丢唤醒回归测试通过；编译通过 |
| 3：生命周期闭合 | R04、R05、R07；实现稳定注册身份、在途引用、可取消等待、统一停止及资源回收 | 注销/回调/关闭交错测试通过；无自等待；ACK 账目一致；所有目标编译通过 |
| 4：集成验收 | 四 CQ、并发 RPC、握手失败、流控耗尽、读写中关闭、fd 复用 | Debug/Release 回归通过；RDMA 实机结果单独记录；未通过项保持待修复 |
| 5：性能优化 | O01；测量线程创建和调度成本，再决定执行器或 bthread 接入 | 有可重复基线和对比数据，功能与退出语义不退化 |

阶段 1、2 的修改依赖阶段 3 的完整退出能力才能作为最终可交付版本；中间步骤不得据此宣称 endpoint 已线程安全。

## 5. 验证方案

### 5.1 纯逻辑与 Linux 事件测试

采用可控屏障、条件变量或测试钩子安排时序，避免仅靠 sleep 或大循环碰撞。尽量验证实际生产逻辑，不复制一份简化算法当作测试对象。

| 对应问题 | 必须覆盖的场景 | 判定标准 |
|---|---|---|
| R01 | CAS 成功释放；CAS 失败后继续；交接瞬间到达事件；任务启动失败 | 同一时刻有效消费者不超过一个，事件不被永久遗留 |
| R03 | 发布队头后暂停连接 next；多生产者提交；消费时失败；关闭期间提交 | 请求不丢、不重复释放，失败清理覆盖所有已接受节点 |
| R04/R05 | 回调开始前/执行中注销；重复停止；握手失败；fd 快速复用 | 无失效对象访问、无自等待、旧事件不影响新连接 |
| R06/R07 | 四队列通知状态模型；事件数 0、1、127、128、129 | 模型不遗漏工作；已获取事件最终恰好 ACK 一次 |
| R08 | 条件检查与进入等待之间恢复窗口；停止时唤醒；低于阈值的可写恢复 | 所有需要退出或继续发送的等待者均能推进 |

Linux 事件测试可使用 eventfd/socketpair，不调用 RDMA 接口。四 CQ 与 ACK 的纯逻辑测试只能验证控制协议，不能替代 verbs 行为验证。必要时对相关纯逻辑测试运行 ASan/TSan，明确工具覆盖范围。

### 5.2 编译与既有回归

实施时先记录既有失败，避免将基线问题误判为本轮引入。使用独立构建目录，分别配置 Debug/Release，编译当前全部目标并运行 CTest。例如：

```bash
cmake -S . -B /tmp/light-rpc-endpoint-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/light-rpc-endpoint-debug -j
ctest --test-dir /tmp/light-rpc-endpoint-debug --output-on-failure

cmake -S . -B /tmp/light-rpc-endpoint-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/light-rpc-endpoint-release -j
ctest --test-dir /tmp/light-rpc-endpoint-release --output-on-failure
```

上述为执行命令示例；实际使用的构建目录及结果记录在第 7 节。现有 fast_bthread 构建仅覆盖部分基础源文件，不能以其编译成功推断完整调度核心已经可用。

### 5.3 RDMA 实机集成与性能

- 单连接并发发送、多连接并发注册，以及不同消息大小覆盖控制和数据 QP。
- 各 CQ 的 CQE 分别落在轮询、开启通知和退出交接窗口；验证所有工作最终完成。
- 流控耗尽后恢复，最后一批完成后无更多流量，确认不会依赖下一条请求来唤醒。
- 握手失败、对端异常、读写中关闭、重复建连断连；检查有界退出、请求结果和资源释放。
- 少量事件即关闭，验证不足 ACK 批量阈值时能够完成销毁。
- 性能记录吞吐、P50/P99 延迟、CPU、线程创建数和上下文切换；固定硬件、消息大小、连接数和并发度。

没有 RDMA 环境时，应标记“纯逻辑及编译验证完成，实机验证待完成”，不能标记整体修复验收通过。只有在两种实现均正确完成工作时，吞吐和延迟对比才有意义。

## 6. 源码与参考资料

- [endpoint 实现](../src-endpoint/fast_rdma_endpoint.cc)、[头文件](../inc/fast_rdma_endpoint.h)。
- [事件分发实现](../src-endpoint/event_dispatcher.cc)、[头文件](../inc/event_dispatcher.h)。
- [channel](../src-endpoint/fast_channel.cc)、[server](../src-endpoint/fast_server.cc)。
- brpc 本地参考：`/home/syt/Desktop/brpc/brpc/src/brpc/socket_inl.h` 中 `MoreReadEvents`；`socket.cpp` 中 `StartWrite`、`IsWriteComplete`、`KeepWrite` 和失败处理。实施时固定对照版本，检查相关依赖。
- [libibverbs：ibv_get_cq_event / ibv_ack_cq_events](https://man7.org/linux/man-pages/man3/ibv_get_cq_event.3.html)：阻塞/非阻塞、ACK 配对及销毁等待。
- [libibverbs：ibv_req_notify_cq](https://man7.org/linux/man-pages/man3/ibv_req_notify_cq.3.html)：一次性通知及 solicited 条件。
- [NVIDIA RDMA 编程手册](https://docs.nvidia.com/rdma-aware-networks-programming-user-manual-1-7.pdf)，§4.6.7～4.6.10：开启通知、既有 CQE、事件获取及完成处理。
- [Linux epoll 文档](https://man7.org/linux/man-pages/man7/epoll.7.html)：ET 使用约束及事件批次中的对象生命周期。

## 7. 执行记录

| 日期 | 内容 | 验证状态 |
|---|---|---|
| 2026-09-27 | 建立问题清单、整体方案及阶段计划；未修改运行时代码 | 静态审查；编译、回归及 RDMA 实机验证留待实施阶段 |
| 2026-09-28 | R01：恢复事件交接、补齐 CQ 读取/轮询失败的停止状态及线程创建异常处理 | 6 个纯逻辑测试通过；Debug/Release 全量构建及 CTest 通过；RDMA 实机待验证 |
| 2026-09-28 | R02：completion channel 非阻塞设置、EINTR 重试、初始化错误检查与回滚 | 新增 4 个 Linux fd 测试；Debug/Release 全量构建及 CTest 通过；RDMA 实机待验证 |

### 7.1 R01 修复内容

对照 brpc 版本：`d688e7550be4b4c41b9a4dc55add2a2c75be1296`。本项目本轮修改前 HEAD：`0465813`。

- `MoreReadEvents` 恢复 `!compare_exchange_strong(...)`。CAS 失败后更新 progress 并继续处理，成功后退出 CQ 处理，不再访问该轮共享状态。
- 将事件接收判断提取为私有 `AddReadEvent`，由实际回调和纯逻辑测试共用；恢复原版 `fetch_add` 的 `acq_rel` 内存序。
- 两处 `GetAndAckEvents` 失败和 `ibv_poll_cq` 返回负数的路径调用 `StopCqPolling`：设置停止标志，通知发送等待者，保留占用中的事件计数，阻止失败后重新获得处理权。
- 停止标志在 `send_mutex_` 下更新，再通知条件变量，使这条新增错误路径的停止通知与等待检查同步；正常窗口更新和析构路径的 R08 问题仍待对应修复。
- CQ 线程创建异常被捕获，标记停止并回退 `_running_threads`，避免异常直接逃出事件循环或遗留虚假的运行线程计数。
- 保持现有 CQ 扫描与资源释放结构。本轮没有修改 R02 的阻塞设置、R06 的重新开启通知流程或 R04 的回收顺序。

### 7.2 R01 验证与限制

新增 [test_rdma_event_ownership.cc](../test/unit/test_rdma_event_ownership.cc)，直接测试生产事件协议，不创建或操作 RDMA 资源：

1. 处理结束释放所有权，下一事件可以启动新消费者。
2. 处理期间到达的多个事件由当前消费者继续处理。
3. 用 promise/future 固定事件先于交接到达的时序。
4. 用 promise/future 固定事件晚于交接到达的时序。
5. 失败停止后不再启动消费者，重复停止保持该状态。
6. 已停止的 endpoint 不取得所有权，实际事件回调直接返回。

前四项在未修复 CAS 返回值时全部失败，修复后六项全部通过。Debug/Release 分别完成全量构建，均通过 82 项单元测试及三种日志模式各 13 项测试。

实际构建目录：`/tmp/light-rpc-errno-build`（Debug）、`/tmp/light-rpc-errno-release-build`（Release）；两者均重新运行 CMake 配置以纳入新增测试文件，再执行构建及 CTest。

验证边界：尚未注入真实的 pthread 创建失败，也没有调用 verbs 复现 CQ 错误；相关分支通过源码审查确认接入停止处理。RDMA 实机验证留待依赖问题修复后进行。

`StopCqPolling` 只关闭后续 CQ 调度并设置停止状态，不等价于 brpc 完整 `SetFailed`：拒绝所有新请求、通知全部未决 RPC、注销并同步在途回调、安全回收资源，仍依赖 R03/R04 等后续修复。线程创建失败在当前 std::thread 适配下进入停止状态，不在 EDISP 上同步执行可能阻塞的 PollCq。不能据此宣称 endpoint 已完成完整故障恢复或线程安全验收。

### 7.3 R02 修复内容

本轮修改前 HEAD：`7689a40`。对照 brpc 同一版本的 `rdma_endpoint.cpp::AllocateQpCq`、`butil/fd_utility.cpp::make_non_blocking` 以及 libibverbs 接口文档。

- completion channel 创建后立即调用私有 `SetNonBlocking`，成功后才创建 CQ/QP。该函数保留原版 `F_GETFL → 检查 O_NONBLOCK → F_SETFL(flags | O_NONBLOCK)` 实现语义，保留其他状态位，失败返回原错误。
- `GetAndAckEvents` 遇到 `EINTR` 重试，`EAGAIN` 结束通知获取；其他错误保留 errno 返回，由 R01 的调用方进入停止处理。正常的批量 ACK 阈值不变。
- `AllocateResources` 检查 channel、四个 CQ、两个 QP 的创建结果；原先局部创建辅助函数中的进程级 CHECK 改为向调用方返回失败，符合 brpc 资源申请路径的错误处理方式。
- 检查首次开启通知和事件注册的结果；首次通知错误码按 `ibv_req_notify_cq` 的返回约定传递，缓冲区分配失败返回 ENOMEM。
- 复用现有 `MakeScopeGuard`，在初始化失败时清理已创建资源，并在需要时撤销事件注册尝试；保存和恢复 errno，避免日志、注销及清理覆盖原始错误。注册成功后直接撤销回滚守卫并返回，不再执行其他初始化步骤。
- 两个握手入口已经检查 `AllocateResources() < 0`，本次保持错误向上传递。握手失败之后的 owner 通知、线程退出和 endpoint 回收仍由 R04 处理。

回滚适用范围：尚未完成握手、QP 仍为 RESET、尚未投递 WR 的初始化阶段。复用的资源清理按 QP → 关联 CQ → completion channel 处理依赖，不将此回滚用作正在收发数据的 endpoint 关闭方案。运行期间四 CQ 的通知复查属于 R06，事件 ACK 尾数属于 R07，事件上下文的并发回收属于 R04/R05。

### 7.4 R02 验证与限制

新增 [test_rdma_channel_fd.cc](../test/unit/test_rdma_channel_fd.cc)，通过 friend 测试入口调用生产 `SetNonBlocking`，不创建或模拟 RDMA 资源：

1. 保留原有 `O_APPEND`、访问模式和 `FD_CLOEXEC`，重复设置保持不变。
2. 普通 socketpair 上无数据及已有数据被取完后，读取返回 EAGAIN，不阻塞等待下一条数据。
3. 无效 fd 在 F_GETFL 阶段返回 EBADF。
4. Linux O_PATH fd 允许 F_GETFL，但 F_SETFL 失败时错误正确返回，原 flags 不被修改。

Debug/Release 均重新配置并完成全量构建，分别通过 86 项单元测试及三种日志模式各 13 项测试；包含 R01 的全部交接回归。构建目录继续使用 `/tmp/light-rpc-errno-build`、`/tmp/light-rpc-errno-release-build`。

当前环境不存在 `/sys/class/infiniband`，未执行 RDMA 实机验证。受 `CLAUDE.md` 的 RDMA 单元测试约束，本轮未为 `ibv_*` 调用编写 mock 测试；CQ 通知获取中的 EINTR、资源申请失败回滚、初次通知与 epoll 注册失败路径已完成源码检查，实际硬件故障注入仍待验证。不能将普通 fd 测试等同于整个 CQ 处理链路通过验收。
