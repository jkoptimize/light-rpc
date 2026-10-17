# 大消息 RPC 使用多个 RDMA Write 的可行性分析

日期：2026-10-11。基线：light-rpc `193242810e746bc3924c80e2dd204c00c6ae4e0c`；静态分析，未修改运行时代码、未做硬件验证。对应 [对齐审计 D18](brpc-alignment-audit-2026-10-11.md)。

2026-10-14 更新：明确本地 SQ/RQ 容量等待、Authority 许可粒度、最后消息的消费权边界及 IOBuf 有效长度。以下为实施约束，运行时代码尚未整改。

## 1. 结论与边界

现有“一次 Notify → 一次 Authority → 一个连续远端 MR → 一次整消息交付”的协议可以承载多个 Write WR。当前实现不能直接使用多 WR：接收长度、发送完成、SQ 容量和部分提交失败的处理都假定一次 Write 对应一条消息。

推荐保持一个 RPC/correlation ID、一次 MR 授权：前 N−1 段用普通 RDMA_WRITE，最后一段用 RDMA_WRITE_WITH_IMM。所有段按该传输的顺序提交到同一个 RC data QP，远端地址为授权基址加累计字节偏移。最后一段带当前使用的 rkey immediate 标识及 SOLICITED 标记，接收端只在它成功完成时交付整帧。

这不是把一个 RPC 拆成多个 RPC，也不是按网络 MTU 手工拆包。超过的是单个 WR 的 SGE/消息长度能力，NIC 对每个 WR 的网络分包仍由传输层完成。可继续使用现有 Notify 总长度、Authority 基址/rkey 和 rpc_id 字段，无需为每段增加 Notify/Authority 或在线分片编号。两端实现语义必须同步升级，不能默认旧接收端兼容。

## 2. 源码中的单 Write 假设

| 位置 | 当前行为 | 多 WR 所需调整 |
|---|---|---|
| `CutSegFromIOBuf` | 最多 MAX_SGE=32 个 BlockRef，仅提交一次 Write With Immediate | 使用 BlockRef 游标与偏移覆盖完整 IOBuf；每段遵守有效 max_send_sge 和消息字节上限；不是无条件只分两段 |
| `CreateQp / AllocateResources` | data SQ/RQ 深度请求值为 8；两 data CQ 也请求 8；全局设备 max_sge 与局部 MAX_SGE 不是一个限制 | 保存创建后实际 QP 能力，明确本地数组上限；SQ 在途 WR 信用与大消息数量分开，CQ 容量一并核对 |
| 双端 Notify handler | 已收到 data_total_len，但 pending_large_map 只保存 mr；PostLargeWriteRecv 返回值被忽略，仍发送 Authority | 本地 RQ 名额不足时等待；成功预留、准备接收 WR 并登记上下文后才发布 Authority；检查非零错误码。末段才带 immediate，一条消息只需一个通知 receive WR |
| `PollCq` data_recv 分支 | 根据 immediate rkey 取 MR，只将从 mr->addr 起的 `wc.byte_len` 字节纳入 IOBuf，并删除 map entry | 末次 byte_len 不是多 Write 总长度；确认整帧完成后用经过校验的整帧长度构造 IOBuf。拆帧仍按包头 total_len 判断；不能用 mr->length 代替消息长度 |
| `PollCq` data_send 分支 | 每个成功 CQE 都 ReleaseLargeFrame(rpc_id) 并减一次 active_large_transfers | 区分批次/SQ 信用完成和整消息终结。中间完成只归还对应 WR 信用；整消息最终成功才回收源 IOBuf、归还一次大消息名额 |
| 双端 Authority handler | CutSegFromIOBuf 返回负值立即释放整帧和名额，非负即视为已安排成功 | 多段可能部分已提交，禁止在后续提交失败时立即释放全部源内存；错误必须传播到统一传输失败/资源回收流程 |
| `GetLargeFrame` | 容器解锁后返回裸 IOBuf 指针 | 发送状态拥有稳定的 IOBuf 与 Endpoint 引用；不在跨等待时使用可能被超时/完成删除的裸指针 |

## 3. 推荐发送和完成协议

以 48 个有效 SGE、每 WR 上限 32 为例：

```text
Notify(total_len=L) → 对端登记完整接收 MR → Authority(addr, rkey)

WR1: RDMA_WRITE
     本地 SGE 0..31 → addr

WR2: RDMA_WRITE_WITH_IMM
     本地 SGE 32..47 → addr + WR1字节数
     immediate=rkey，SOLICITED

接收端：最终 receive CQE → 查记录 → 交付 L 字节，只交付一次
发送端：最终 send 完成被确认 → 回收整帧，只归还一次消息名额
```

data QP 发送推进借鉴 brpc 单写者、部分发送游标和 butex 容量等待；复用已移植基础设施，不另造线程池，也不因 CQ 消费权问题强制新增独立发送任务。此处只借鉴调度/队列机制，多 Write 的协议证明属于项目扩展。

不能把“8 条大消息”继续当成“最多 8 个发送 WR”：8 条各分两段的消息最多需要 16 个 WR。增加深度为 16 也不能解决任意段数和持续并发。保留消息名额用于限制逻辑传输，另外维护 data SQ 已提交未确认的 WR 信用。

中间 Write 是否 SIGNALED 是本地完成策略，独立于最后一段携带 immediate 的远端完成通知。推荐以可用 SQ 信用构造有界提交批次，每批尾 WR 必须 SIGNALED，最后消息段也必须 SIGNALED；前段可 unsignaled。同一 data SQ 的后续成功完成可以确认其前面的 unsignaled 工作，按提交序号归还该批信用。

例如只剩 8 个信用但消息需要 10 段：前 8 段形成一批，第 8 段普通 Write 设置 SIGNALED；确认后继续提交第 9、10 段，第 10 段才带 immediate。不能前 9 段全部 unsignaled 而等第 10 段提供唯一完成，导致 SQ 填满时没有可用的完成检查点。

发送完成标识必须能定位批次/提交序号以及是否为某传输的最终段，不能继续把每个 wr_id 一律当成“整个 RPC 完成”。上下文先发布再 post，防止完成先于本地登记；池化/版本引用可复用现有设施。CQ 消费者不能持消费权等待自己需要处理的 data send 完成：它处理完成、归还信用并唤醒提交者。

上述等待边界不是当前代码已经存在的循环等待：当前 `ProcessNewMessage` 为消息启动独立线程，`OnProcessResponse` / `OnProcessRequest` 不持有 PollCq 消费权。后续按 brpc 移植时，正常收尾在 `MoreReadEvents` 返回 false、释放消费权后，再内联执行最后一条完整消息；返回 true 则继续消费。遵守这个顺序，消息处理中的容量等待不会阻止其他 CQ 任务处理完成，不需要为此额外拆出发送任务。失败出口仍须按原版失败生命周期处理，不能误称所有出口都会调用 MoreReadEvents。

`BTHREAD_NOSIGNAL` 配合批量 `bthread_flush` 用于减少逐任务通知/唤醒开销，不保证 flush 前任务绝不执行；保留最后一条消息到正常收尾内联处理，可省去一个消息 bthread 的创建与调度。内联对象是完整消息，不是最后一个 CQE。

### 3.1 data SQ/RQ：本地容量等待，不增加远端窗口 ACK

control QP 可以直接发送，需要跟踪远端 RQ 窗口；data QP 则逐消息等待 Authority。保留这一许可协议，不复制 control QP 的远端窗口和 ACK 报文。分别管理以下资源，不共用计数：

| 资源 | 计量单位 | 归还时机 |
|---|---|---|
| 逻辑大消息名额 | 一条传输 | 整帧发送完成或安全失败清理 |
| 本地 data SQ 容量 | 已预留/提交但尚未归还的 Write WR | 成功发送完成确认对应提交范围；失败按安全清理协议处理 |
| 本地 data RQ 名额 | 为大消息准备的接收 WR | 最终 Write With Immediate 接收完成被消费，不等业务处理结束 |

Authority 许可一条完整消息，不许可固定数量的 Write WR。即使两端队列深度均为 8，一条获准消息也可能拆成 10 个 Write：远端只需一个接收 WR，本地 SQ 却需分批推进。因此不能用 Authority 或两端容量相等证明 SQ 永远不会满。必须有 WR 容量核算，但可由已有提交/完成序号计算在途量，无需同时维护表达同一含义的冗余计数器。

接收端流程：Notify → 原子地预留本地 RQ 名额（无名额则 bthread 等待）→ 分配 MR → 成功 post_recv → 登记接收上下文 → 发布 Authority。最终接收完成时取出上下文、归还 RQ 名额、唤醒等待者并交付整帧。容量等待放在分配大块内存之前；RQ 名额只限制通知接收资源，不是业务持有 MR 的总内存上限。

发送端容量不足时同样通过 bthread 等待，由 data send 完成归还 WR 容量并唤醒；继续采用前述有界批次和批尾 SIGNALED，避免 SQ 填满前没有完成检查点。

预留采用既有单写者协议或受保护的检查/占用，不能多个处理任务先独立判断有容量再分别递增。复用已移植 butex 的检查、等待和唤醒协议，避免丢失唤醒；失败/关闭必须唤醒相关等待者。等待不持 RPC ID 锁，也不持 CQ 消费权。不要通过反复 post 到报错探测队列容量；预留后发生真实 verbs 投递错误走统一失败处理。post_recv 返回非零即失败，不能只检查负数；已投递的接收 WR 及其资源不能在后续发布失败时直接释放，须遵循安全清理边界。

### 3.2 IOBuf 有效范围与包头长度是两个层次

当前 `CutInputMessage` 已按包头 total_len 检查完整性，没有把 wc.byte_len 当作协议包头长度。问题在其上游：`append_user_data_with_meta(mr->addr, wc.byte_len, ...)` 只描述从 MR 起点开始的这段有效字节，读取包头不会自动扩大 IOBuf。

例如 96 KB 消息分 64 KB 和 32 KB 两次 Write，末次完成 byte_len 为 32 KB；即使 MR 中已有全部数据，旧代码仍只引用开头 32 KB。拆帧看到 total_len 为 96 KB 后会等待更多字节，而不是立即调用 protobuf 解析；后续别的消息追加进来还可能被错误拼接。

最终通知确认完整写入后，以经过校验的完整长度构造 IOBuf，不复制数据。优先保存已有 Notify 总长度并与包头核对；也可在确认完成后读取 MR 包头并校验边界，具体最小上下文布局在实现时确定，不强制增加冗余长度字段。MR 缓存容量可能大于消息长度，不能直接用 mr->length。现有 ProcessNewMessage 的按包头拆帧机制保留。

## 4. 失败和资源所有权

- 提交前尽量完成全帧分段、地址范围和 lkey 的有效性检查；失败尚未 post 时可按未提交操作清理。
- `ibv_post_send` 返回错误不意味着整条 WR 链均未提交；bad_wr 指示第一个失败 WR，其前缀可能已经交给硬件。逐次 post 时同样可能前几段成功、后续失败。
- 初期沿用连接级不可恢复发送错误进入 SetFailed 的策略，不另加分段重试。已提交前缀所引用的内存保留到相应完成得到确认或既定连接 teardown 安全边界；不立即 ReleaseLargeFrame。
- 失败时如果最终通知未提交，对端不会收到完整消息通知，但 MR 仍可能被此前提交的段访问；必须通过既有连接失败与回收协议释放，不能仅按用户 RPC timeout 回收。
- RPC 已超时不等于该传输可以释放；迟到的完整大响应只在完整接收后按 correlation ID 判为过期。成功完成也不等于服务端业务已经执行完成。
- 不逐段清空同一个源 IOBuf 后丢掉所有权；最小实现保留整帧到传输结束，游标只推进提交位置。

## 5. 规范依据与适用前提

已读取 [InfiniBand Architecture Vol.1 Release 1.0.a](https://pllab.cs.nthu.edu.tw/~kyshieh/course/cs5404/page1/page5/files/vol1r1a2.pdf)：

- §9.5，235～236 页，C9-25/C9-28/C9-30/C9-32 与 o9-20：同一 QP 的请求提交、执行和完成顺序，以及通过带 immediate 的最终 Write/后续消息确认远端数据可用的条件。
- §10.8.6，453 页：同一 SQ 的后续完成可用于确认之前的 unsignaled WR。
- §11.4.2.1，533～534 页：接收完成的字节数属于对应的 Send 或 Write With Immediate，不是应用多次 Write 的累计长度。
- 架构概述 102 页：Write With Immediate 消耗 receive WQE；数据写入远端授权地址，而不是该 receive buffer。

这是历史规范中基本 RC/Write 的规则，不是对当前所有扩展的完整规范审计。本方案限定所有片段及最终 immediate 在同一个 RC data QP；不使用另一条 control QP 上的消息来推导数据写入顺序。当前 `LargeBlockAlloc` 未启用 relaxed ordering；不在本轮顺带改变 MR 属性。纯 Write 链不为“更保险”新增 READ/ATOMIC fence。

Linux API 另核对 rdma-core：[post_send](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_post_send.3) 的部分提交、返回错误码和缓冲寿命；[post_recv](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_post_recv.3) 的非零错误码和投递失败；[create_qp](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_create_qp.3) 的实际能力回填；[poll_cq](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_poll_cq.3) 的完成标识和字段有效性。最终接收还须验证 WC opcode/status/IMM 标记，错误 WC 不按成功字段解析。

## 6. B5 验收范围

纯逻辑测试覆盖 SGE 上限前后、跨 BlockRef 偏移、字节完整覆盖、总长度不等于 MR 容量、窗口分批、终态只归还一次。调用 verbs 的路径只做硬件集成：两段、多于 SQ 深度的段数、多 RPC 并发、partial post failure、用户超时早于完成、最后一段通知一次、客户端请求和服务端响应双向验证。

补充验收：RQ 名额耗尽时不发布新 Authority，归还后恢复推进；并发预留不超额、检查与等待交界不丢唤醒、失败/关闭唤醒全部相关等待者；96 KB/64+32 KB 示例验证整帧 IOBuf 长度及包头检查。最后消息内联发生在正常释放 CQ 消费权之后；SQ 满等待时 CQ 仍可推进。纯计数/调度逻辑与真实 verbs 的硬件验证分开，不能把两端深度相等当作免测依据。

本文件确定可行方向及必须改动的协议边界；具体提交上下文布局、SQ 信用算法与 teardown 验证在 B5 实施前细化。不能仅给 CutSegFromIOBuf 添加循环后宣布支持多 Write RPC，也不能把超过 MAX_SGE 的正常消息当作永久拒绝的最终方案。
