# RPC 接入 bthread 前的 brpc 对齐审计与整改约束

日期：2026-10-11。状态：实施前审计，尚未执行本文运行时代码整改。

## 1. 依据、范围与证据等级

- light-rpc：`193242810e746bc3924c80e2dd204c00c6ae4e0c`。审计开始时只有 `rpc-bthread-integration-plan.md` 存在工作区修改；保留其中“状态由 FastController 承载、非空专用 Controller”的修改。
- brpc：本机 `/home/syt/Desktop/brpc/brpc`，`d688e7550be4b4c41b9a4dc55add2a2c75be1296`，本次检查工作区干净。
- 本次逐段对照发送队列、连接回调、事件、握手、消息分发、调用完成与关闭主链；复用既有 [bthread 基础设施审计](../../knowledge/bthread-port-audit-2026-10-06.md) 和 [Endpoint 并发修复记录](../problem-fix/rdma-endpoint-concurrency.md)。不是对所有基础设施、内存池和设备 provider 的重新全量审计。
- **已确认差异**：直接来自两边源码。**可推导后果**：给出触发条件和时序，未称为已复现。**性能预期**：仅说明额外分配、锁、调度等机制，没有本轮基准数据。
- 本次没有修改运行时代码、运行故障复现或 RDMA 压测。旧报告中已修复问题不重新标成待修故障。

实现入口见 [更新后的分批方案](rpc-bthread-integration-plan.md)。批次 B0～B7 是本次最新划分。

## 2. 已确认的裁剪边界

1. 基础流程按 brpc 成套移植：队列算法、对象复用、异步回调所有权、ID 完成、事件消费、引用回收；不能只复制正常分支。
2. 保留单连接长连接、事件驱动 CQ、现有双 QP/四 CQ 扩展；暂不拆分 FastRdmaEndpoint 为 Socket 和 RdmaEndpoint 两个类。
3. 暂缓的恢复专指 **后台 HealthCheckTask → WaitAndReset → CheckHealth → Revive**。它不等于暂停失败处理、首次按需连接或长连接复用，也不是授权另造按需恢复算法。同一 Channel 失败后重新建连的具体策略另列未对齐项，本轮不宣称完整恢复等价。
4. 不增加主动取消、远端业务取消、自动重试、backup request、短连接、连接池、SocketMap、polling、用户代码备份线程池、多 tag/dispatcher 池。
5. FastController 为非空、专用类型，承载逐调用状态；移除旧 Channel 附件 API 的兼容要求。保留基本异步 done，先同步闭环、再异步子批次。
6. 根据本轮用户同意前述建议，实施方案按两端统一升级 64 位 correlation ID 编排；不实现 32 位兼容映射或双协议自动兼容。本次只确定方案，未变更协议。
7. 不因移植测试方便而在生产热路径保留一行转发函数；不因 brpc 有通用功能就复制当前协议不用的字段和分支。

## 3. 差异台账

源码位置为本次审计基线，后续以函数名定位。brpc 路径相对于其仓库；light-rpc 链接相对于本文件。

| ID / 批次 | light-rpc 当前实现 | brpc 依据 | 功能或性能影响；整改决定 |
|---|---|---|---|
| D01 / B1 | [StartWrite、KeepWrite、失败排空](../../../src-endpoint/fast_rdma_endpoint.cc) 每请求 `new/delete` | `socket.cpp:306,504,521,1642`：cacheline-aligned WriteRequest、get_object/return_object | 已确认分配策略不同。改用已有 object_pool，补齐复用初始化及成功/失败回收。减少逐次堆分配是机制预期，不能保证 QPS 增幅；对齐原版缓存行布局意图，实测 sizeof/alignof，不复制无用途 packed flags。 |
| D02 / B2 | `_pending_keepwrite_req` 在发送、握手、回收之间 store/exchange；`FailPendingWrite` 与 `BeforeRecycled` 兜底接管 | `socket.cpp:1409,1470,1486,1528,1553`：req 作为连接 callback data，携带 Socket 引用 | 已确认存在额外的 Endpoint 全局所有权槽及原子交接。成套移植 callback + userdata + 唯一完成权，删除成员和兜底路径；不是把原子指针改成裸指针。 |
| D03 / B1 | `PublishWriteRequest` 只包一条 exchange；`WriteError` 只转发 error()，StartWrite 重复检查 | `socket.cpp:1695` 直接 exchange/release、连接 next；写前 ConductError | 按原版在 StartWrite 直接实现发布，删除无独立语义的转发；保留发布后失败检查的实际作用。不能未经反汇编声称一层函数调用必然有性能损失。 |
| D04 / B3 | KeepWrite、CQ、握手与每消息均启动 detached std::thread | `socket.cpp:1486,1786,2234`、`input_messenger.cpp:195`、`rdma_endpoint.cpp:239,301` | 逐任务系统线程创建、pthread 等待占用线程，直接影响调度成本。移植 background/urgent/NOSIGNAL/flush/最后消息边界；各入口保留对应启动失败收尾，不套统一 spawn 包装器。 |
| D05 / B2 | `ReadFromFd/WriteToFd` 遇 EAGAIN 后 usleep(1000)，TCP 回调先注销读写事件；无明确总连接/握手 deadline | `rdma_endpoint.cpp:301,362,390` 的 read butex；`socket.cpp:1253,1290` 的 WaitEpollOut/connect timer | 存在周期轮询及最长等待无统一约束；直接改 bthread 会阻塞 worker。补事件 MOD、read generation、带期限等待、连接操作 timeout 仲裁。原版 50ms 是分段等待，不是整体超时。 |
| D06 / B2 | [OnClientHandshake/OnServerHandshake](../../../src-endpoint/fast_rdma_endpoint.cc) 成功后也 CloseTcpFd | `rdma_endpoint.cpp:273,301,413`：ESTABLISHED 后 TryReadOnTcpDuringRdmaEst，EOF → SetEOF，错误 → SetFailed | **基础连接语义差异**：当前失去 TCP 被动断连感知入口，不能假定空闲 RDMA 连接马上生成错误。成功后保留 TCP fd 和读事件，直到失败收尾/回收；不增加后台探活。这与暂缓健康检查无关。 |
| D07 / B2 | HelloNegotiationValid 接受任意非零 impl_ver；服务端读完 ACK 不检查位值 | `rdma_endpoint.cpp:348` 比较实现版本；服务端握手检查 ACK_MSG_RDMA_OK | 同时升级协议后仍可能接受不兼容实现。严格核对当前实现版本、长度和必要 ACK 位；RDMA 不支持则明确失败，不引入 TCP 数据 fallback。 |
| D08 / B2、B3 | EventDispatcher 只有 ADD/DEL；自己维护 fd→版本 ID 表，std::thread，每个事件先读后写 | `event_dispatcher_epoll.cpp:Start/RegisterEvent/UnregisterEvent/Run`、`event_dispatcher.h:IOEvent` | 缺少握手写等待必需的 MOD。两边都是 32 事件批次，但 brpc 先遍历整批读事件、再遍历整批写事件，移植时保持原顺序与任务属性。fd 表只在注册变更路径加锁，不能误报“每个事件都查表加锁”。单 dispatcher 是范围裁剪。 |
| D09 / B3 | 每消息线程，无 NOSIGNAL 批量启动、flush 和最后消息优化 | `input_messenger.cpp:QueueMessage`；`rdma_endpoint.cpp:PollCq` 的外层 InputMessageClosure | 额外调度和唤醒；按原版批处理。最后消息必须在正常退出 CAS 释放 CQ 消费权后执行。当前逐消息线程实现尚不存在此 inline 优化，不能把潜在移植错误说成当前已发生死锁。 |
| D10 / B0、B3 | CutInputMessage 把非法短长度和不完整帧都返回 false；异步 handler 返回值直接丢弃；多个业务错误也返回 -1 | `input_messenger.cpp:ProcessNewMessage/ParseResult`；`policy/baidu_rpc_protocol.cpp` | 异常帧可能一直留在 read_buf；传输处理失败不能上报。区分不完整、非法帧、调用业务错误和迟到响应。不能把“所有非零返回均 SetFailed”作为修复。 |
| D11 / B4 | [CallMethod/OnProcessResponse](../../../src-endpoint/fast_channel.cc) 栈 PendingRequest + Channel map + std::condition_variable；响应持 map 锁解析 protobuf | `channel.cpp:CallMethod`、`controller.cpp:EndRPC/Join`、`baidu_rpc_protocol.cpp:921` | 同步占 pthread；同一 Channel 的不同 RPC 响应解析被全局锁串行化。逐调用 FastController/bthread_id 完成，ID list 广播失败；不保留 pending_map 作为第二套完成协议。 |
| D12 / B4 | 超时从入队和大消息容量等待之后才开始，done 参数忽略 | `channel.cpp:423,468,596`；`controller.cpp:HandleSocketFailed/EndRPC` | 总调用期限不覆盖前置等待、缺失客户端异步完成。初始化锁保护、timer 用 ID、同步 join/异步 done 共用唯一终态；本地主动取消不纳入。 |
| D13 / B0、B4 | 附件保存在 Channel；响应 payload_len 算出后仍用整个余下 frame 解析 protobuf；服务端 Controller=nullptr | `baidu_rpc_protocol.cpp:ProcessRpcResponse` 先切 payload/附件；Controller 请求/响应状态 | 附件并发不隔离；普通带附件响应解析边界不正确。解析边界先独立修复，随后状态迁入 FastController，服务端传实际 Controller。 |
| D14 / B4 | uint32 rpc_id 自增、手工帧头和 map/WR 参数等多点使用 | brpc correlation_id / bthread_id 为 64 位并带生命周期版本语义 | 不能截断内部 ID，也不能把轮回编号无冲突等同于无迟到响应问题。双端协议、Hello 版本、普通/Notify/Authority 全链一次配套升级；不要求与 brpc wire 格式互通。 |
| D15 / B1、B4 | WriteRequest 只有 data/next，没有 id_wait；失败由 Channel 回调遍历 pending_map | `socket.cpp:ReturnSuccessfulWriteRequest/ReturnFailedWriteRequest/NotifyOnFailed`；`controller.cpp:1230` 的 wopt.id_wait | 补齐发送结果与 ID 的关联、失败时先 SetFailed 再回收通知。采用原版“发送成功后的失败登记”主线；初始化/直接发送失败同样通知 ID，避免同时设计前置登记和队列回收重复登记。无 RPC 关联的内部控制帧使用无效 ID。 |
| D16 / B4 | 没有 `_unwritten_bytes/_overcrowded` 软件发送积压限制 | `socket.cpp:WriteRequest::Setup/Write/CancelUnwrittenBytes` | 传输窗口耗尽时仍可持续堆积用户请求；异步并发会放大内存占用。这是过载场景，不是正常发送正确性前提，但会影响内存和过载性能等价性。B4 补最小原版计数/阈值/拒绝与回退协议，不扩展通用限流器；不移植 HTTP/统计导出。 |
| D17 / B5 | [大消息 handler](../../../src-endpoint/fast_server.cc) 和客户端对应 handler 都在发送 Authority 后才登记 pending_large_map，PostLargeWriteRecv 返回值被忽略 | brpc 普通发送以传输资源拥有缓冲；双 QP Authority 无逐行对应实现 | 可推导快速完成先于登记，以及接收 WR 投递失败后仍发布许可。2026-10-14 明确：先预留本地 RQ 名额，无名额则协作等待；成功准备接收并登记后才发布 Authority。最终接收完成归还名额、唤醒；不复制 control QP 远端窗口/ACK。已投递资源按安全清理边界回收。 |
| D18 / B5 | CutSegFromIOBuf 最多取 MAX_SGE 个 BlockRef 后就提交；调用者仅检查 <0，不验证完整长度 | brpc `RdmaIOBuf::cut_into_sglist_and_iobuf/CutFromIOBufList` 消费部分数据并继续发送 | **代码可推导，未实机复现**：只发送前缀却执行整帧发送回收。现有协议可支持同一 RC data QP 多 Write、末段 immediate，无需分段线上字段。接收拆帧本就按包头 total_len 判断；需改的是 IOBuf 从 MR 引用的有效长度，末次 wc.byte_len 不代表整帧。Authority 按消息许可，不能替代 SQ 的 WR 容量核算，即使两端深度相等。分批 SIGNALED、本地容量等待、完成回收和部分失败成套调整，见 [多 Write 分析](large-rpc-multi-write-analysis.md)。 |
| D19 / B5、B6 | 容量/回收用 std::condition_variable；GetLargeFrame 解锁后返回裸指针，未来 timeout 清理会与使用交叉 | brpc bthread 等待 + IOBuf 引用/Socket 引用；项目双 QP 独立拥有状态 | 等待迁入 bthread 原语；调用完成不能直接等同传输资源释放。检查/占用需同一同步协议；当前 Channel/Server 已在各自 mutex 内执行检查与占用，不能误称完全无锁，重点是检查跨拥有者、完成及停止的同步。 |
| D20 / B2、B6 | OnFailed 主动统一注销 TCP/CQ 事件；BeforeRecycled 回收并通知 Channel/Server；Close 等待回收 | `socket.cpp:OnFailed/BeforeRecycled/HandleEpollOutRequest`，VersionedRefWithId | 用户确认按 brpc 对齐：OnFailed 负责错误记录、唤醒和失败通知，普通连接 TCP/CQ 注册的最终注销放回引用归零后的回收流程；不以类合并为保留提前注销的依据。异步建连操作取得唯一完成权后，单独注销其操作事件并执行回调。与 D02 同批闭合所有权，B6 做组合验收；详见 §5.1。 |
| D21 / 暂缓 | Channel 连接失败后 `closed_/connection_error_` 保持失败，原 EndpointId 无法普通 Address，缺少恢复可用状态的流程 | `details/health_check.cpp`、`socket.cpp:WaitAndReset/CheckHealth/AfterRevived` | 记录恢复不等价；不是要求每次失败都更换 Endpoint/ID，brpc 可通过 Revive 恢复原逻辑对象。后台 health-check→Revive 用户明确暂缓；不自动引入新 Endpoint 世代替代方案。首次连接、失败收尾和服务端接受新的连接仍保留。 |
| D22 / B0、B7 | [压测工具](../../../test/client.cc) volatile stop_flag、数组 delete、无限采样索引、无失败识别；每 pthread 一 Channel | 对照试验方法，不是 brpc 运行时算法 | 基线可能有数据竞争/越界且失败被算入吞吐。先修工具与样本边界；单 Channel 多 outstanding、相同 worker/窗口/批次等配置再比较。不得把连接数或错误路径差异当作 bthread 收益。 |

### 3.1 已对齐的基础不能倒退

- `_write_head.exchange(release)`、`UNCONNECTED`、next 衔接、消费者反转、`IsWriteComplete(acquire)`、保留最后节点直到并发生产者排空：当前骨架与原版一致。删除包装函数不改变它们。
- `_nevent.fetch_add(acq_rel)` 与 `MoreReadEvents` 的 CAS 成功 release、失败 acquire，和原版一致；正常 CAS 成功后不再碰 CQ 消费状态。
- writable butex 已采用“先取 generation、再检查窗口、再等待”、有限等待、wake_except 与失败 wake_all。保持，不换条件变量或 execution_queue。
- Endpoint/IOEventData 已用 VersionedRefWithId，不能回退成 fd/raw pointer 判身份，不能重引入 OperationGate。
- 四 CQ 的 arm 后全量复查、已取通知累计 ACK、意外 CQ 单独 ACK 已有修复，按既有回归保留。

## 4. 三项明确整改的完整协议

### 4.1 WriteRequest 对象池与回收

复用 `src-bthread/butil/object_pool.h`，不再创建私有池。池归还**不调用析构函数**：

- 每次获取后显式设置 next=UNCONNECTED、关联 ID、必要的 Endpoint 指针/控制状态；不能依赖默认成员初始化在复用时重新执行。
- 成功回收先确认 data 已被消费；失败回收先 clear data，再 return_object。
- 回收前保存通知所需 ID/状态；return_object 后不得读取 request。
- 异步链持有的 Endpoint 引用是原版一次转移的引用，不能无意给每个排队节点增加一个智能指针，从而在热路径增加每请求 ref/unref。
- 不机械复制 Redis/memcache pipelined_count、auth、SocketMessage、shutdown_write、stream 等字段；保留当前单协议使用的 data、next、id_wait 和异步链的 Endpoint 指针即可。
- B1 先完成分配策略/所有回收点，B4 再启用 id_wait 通知。每批都能独立运行。

### 4.2 用异步连接回调传递首个 WriteRequest

目标调用链对应 brpc：

```text
StartWrite 取得首写者资格
  → ConnectIfNot(deadline, req)
      → req 关联一份 Endpoint 引用
      → 非阻塞 Connect(callback=KeepWriteIfConnected, data=req)
          → TCP 成功：检查 SO_ERROR、安装 fd/事件
          → 握手任务(callback=AfterAppConnected, data=req)
              → 成功：KeepWrite(req) 接管
              → 失败：SetFailed → ReleaseAllFailedWriteRequests(req)
```

约束：

1. 没有接管异步操作的同步错误，由 StartWrite 清队列并释放局部引用；异步接管后由唯一完成回调负责。返回码必须明确区分这两个阶段。
2. 采用 brpc EpollOutRequest/临时版本化 holder 的完成仲裁，成功事件和连接 timer 争夺同一完成权。已有 VersionedRefWithId 可复用；若 holder 使用当前合并类，要避免分配实际 RDMA 资源。不得为了少一个类而省略唯一完成和回调生命周期。
3. 原版本来就有 `EpollOutRequest::data` 和 `RdmaConnect::_done/_data`。删的是冗余的 Endpoint `_pending_keepwrite_req` 全局槽，**不是宣称异步连接无需任何上下文/变量**。
4. 事件 user_data 必须有稳定的版本身份或被引用保护的拥有者；不能把 `req` 裸指针直接塞入 epoll 后任意归还池。
5. 按 D20 移除当前 OnFailed 对普通 TCP/CQ 注册的统一注销，最终注销归入回收流程；连接操作自身则按原版在取得唯一完成权后注销其操作事件，再执行回调。无论主 Endpoint 是否失败，已接管的连接操作都必须恰好一次完成，不能因普通 Address 拒绝失败 Endpoint 就跳过 req 清理。不能继续依赖 BeforeRecycled 清 pending 来打破 req 引用与回收的循环依赖。
6. 支持回调在启动函数返回前执行完成。成功移交后启动者不得再读 req；注册 timer 与事件竞速必须保留原版持引用到作用域结束的协议。
7. 核对所有分支：连接同步失败、注册失败、timer 注册失败、SO_ERROR、握手失败、超时、Close、任务启动失败、成功续写。每个 request/引用/登记资源只有一个终结者。

连接总 deadline 与调用 deadline 的组合必须对照 `Controller::IssueRPC` 的 connect_abstime 计算：启用 connect_timeout 且 RPC 有 deadline 时，取 `min(开始时间 + connect_timeout, RPC deadline)`，并非新造统一“共享连接必不受首调用期限影响”的承诺。原版该参数首先约束 TCP connect；握手分段 50ms 等待不能据此冒称已覆盖整个握手。项目若补总握手期限，须明确它是有界等待的适配及共享连接失败影响。本轮不引入主动取消。

### 4.3 内联发布与测试适配

StartWrite 直接使用原版 exchange/release、prev_head 判断和 next 写入。这里“内联”指把逻辑写在原位置，不是仅给包装器加 `inline`。

现有 `test/endpoint_write_queue_test.cc` 依赖 PublishWriteRequest、pending 成员并 new/delete 测试节点，须同批更新。保留“发布后尚未连接 next”“反转 FIFO”“失败中并发提交、缓冲只释放一次”等真实协议测试。测试 peer 可以访问必要状态，不为测试保留生产转发函数；异步连接所有权的验证迁移到 callback-data 路径。

## 5. Endpoint/Socket 合并的保留条件

本轮不拆类；类名和层数不是性能等价的证明。合并后必须保留原版的职责边界：

- 首写者/KeepWrite 维护发送链；CQ 消费者维护 CQ/read buffer；连接任务维护握手；ID 维护调用终态；版本引用维护对象寿命。
- brpc 主 Socket 和 CQ `_cq_sid` 对应不同事件消费对象、各有自己的事件计数。保留 TCP 后**不能把 TCP 和 CQ 两类事件混用同一个 `_nevent`**。可以在同类内保留两个明确事件状态或采用已有 IOEvent 拥有者；这种状态来自原版独立实体，不是多余变量。
- 连接已建立后保留 TCP EOF/错误处理。主动建连侧已收到消息与 EOF 的先后处理应核对原版 PostponeEOF/CheckEOF；不要只搬 SetFailed 而丢掉响应完成边界，也不要凭此新增通用流式协议支持。
- 不加覆盖整个 Endpoint 的大锁；注册变更的短锁不等于保护所有传输访问。
- 删除每个状态字段之前都说明由原版哪项协议替代。`_handshake_ok`、`_stop`、错误码等可以随连接状态机审计整合，但不能仅因“变量多”直接删除。

### 5.1 D20：失败通知与事件注销按 brpc 分层

2026-10-11 用户确认本结论。目标顺序为：

```text
普通连接失败
  → SetFailed 使版本失效
  → OnFailed 记录错误、唤醒等待者、广播调用失败
  → 已持引用的任务完成收尾并释放引用
  → BeforeRecycled 及其资源回收链注销普通 TCP/CQ 事件
  → 关闭 fd、释放资源、通知拥有者

异步建连操作完成（成功/错误/超时）
  → 取得该操作的唯一完成权
  → 注销该操作自己的等待事件
  → 回调接续握手/KeepWrite，或执行失败排空
  → 释放操作上下文及其引用
```

brpc `Socket::OnFailed` 不统一注销普通连接的监听；`BeforeRecycled` 在关闭 fd 前 RemoveConsumer。`HandleEpollOutRequest` 则在取得临时连接操作的唯一完成权后注销自己的等待事件，这两个生命周期不能混为一谈。

普通事件入口在失败版本上不能再通过 Address 获得 Endpoint；已有引用保证在途任务仍可安全收尾。尚未完成的独立连接操作必须保留原版的完成与持引用协议，不能简单套普通事件的失败过滤而丢掉回调。失败通知负责唤醒等待者，不能依赖未来普通读事件才能退出。

此项与 D02 的连接回调、事件注册拥有者调整一起实现，不单独删两条 UnregisterEvent。验证失败后新普通事件不启动工作、旧事件和在途回调有引用保护、连接回调恰好一次、等待者能退出、引用最终归零、注销先于 fd 关闭。服务端停止接受新连接时的监听 fd 注销仍按 Acceptor/Server 停止流程处理，不受普通连接注销时机调整影响。

SetFailed 不等待引用排空，但内部通知及最后引用释放触发的回收可能同步执行，不能描述成严格非阻塞或无锁。若后续发现当前目标场景确需提前注销，须先记录具体触发条件、原版不能满足的原因和功能/性能影响，不能仅以“类合并适配”作为偏离依据。

## 6. 允许裁剪与需要单列的性能差异

| 内容 | 本轮处理 |
|---|---|
| 短连接、连接池、SocketMap、重试/backup、命名服务、负载均衡 | 范围外，不复制专属字段、版本范围与控制分支 |
| HTTP/SSL/stream、Redis/memcache positional pipeline、多协议探测、鉴权 | 当前单协议不使用，不为还原 WriteRequest 的完整外形而移植 |
| TCP 数据 fallback | 不支持；与保留 TCP 断连感知区分 |
| 主动取消/NotifyOnCancel 通知链、旧 Controller/附件 API、旧 32 位协议兼容 | 不新增；实现 protobuf 基类时明确不支持的契约，不给出虚假成功 |
| bvar、tracing、contention profiler、动态 gflags、多平台兼容 | 可裁剪；原算法参数与校验不能随配置框架删除 |
| bthread_id、ID list、TimerThread、object_pool、版本引用、消费权协议 | 核心基础设施，不裁剪、不平行自造 |
| 软件发送积压阈值 | 只移植原版最小机制，记录额外计数成本；不加服务治理限流体系 |
| 双 QP、四 CQ、大消息 Authority 协议与内存管理 | 保留项目扩展，独立分析；不能把其成本归为 bthread 开销或宣称与 brpc 相同 |
| brpc QP/CQ 资源缓存、内存注册池与本项目资源分配 | 已定位结构差异，但本轮未完成逐项资源缓存审计；暂不移植缓存体系，连接建立/回收性能不得称等价，B7 分开冷启动与稳态 |

理论性能接近的验收是“热路径是否仍有可避免的逐请求 malloc、线程创建、全局串行化、重复引用和额外等待”，不是只看算法复杂度。记录 WriteRequest 大小/对齐、池复用、每消息创建的任务数、共享锁位置、窗口和批次参数。最终吞吐/延迟仍须实测。

## 7. RDMA 证据范围

本次只把已观察到的项目扩展问题登记入整改，不重新设计 verbs 操作或 teardown 顺序。D18 的前缀截断由 C++ 循环和调用者返回值判断即可推导，不依赖对网卡行为的猜测。

- 已读取 [InfiniBand Architecture Vol.1 Release 1.0.a](https://pllab.cs.nthu.edu.tw/~kyshieh/course/cs5404/page1/page5/files/vol1r1a2.pdf) §11.4.2.2（印刷页 535～536）：通知一次触发，开启通知前已有的完成项不因此触发通知；据此保留 arm 后复查。这是可获取的历史版本，不能称为当前完整规范审计。原方案的 1.2.1 副本本次仍返回 403。
- Linux API 按 rdma-core 原始手册核对：[post_send](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_post_send.3)、[get/ack CQ event](https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/man/ibv_get_cq_event.3)。前者区分 WR 错误返回值与普通 errno，并限定发送缓冲复用时机；后者要求成功获取的事件逐一对应 ACK，允许批量确认。
- 当前部分 `ibv_post_send` 调用只返回 -1 而不保存 API 返回的错误码，`PostLargeWriteRecv` 返回值又被 handler 忽略。B5 核对每个 wrapper 的错误契约，不能使用陈旧 errno，也不能对非零正错误码只做 `<0` 判断。
- 后续若修改分段发送、QP 状态或 MR 回收，必须补相应规范条款和硬件验证；本次没有据此宣称跨 QP 顺序或 teardown 全部正确。

## 8. 实施与验收记录规则

2026-10-18：B1 完成 D01、D03：WriteRequest 使用已有对象池、每次获取重置 next、成功/失败分别归还、缓存行对齐；StartWrite 内联发布，移除 WriteError 纯转发。队列算法与内存序保持，pending 槽留 B2。9/9 写队列回归与 7/7 CTest 通过，详见 [批次实施记录](batch-implementation-log.md)。

2026-10-17：开始 B0，D10 的拆帧结果分类、D13 的响应正文/附件边界、D22 的工具基础修复已落地并通过构建及回归；这些条目的后续批次部分仍待实施。硬件基线未采集。逐项证据见 [批次实施记录](batch-implementation-log.md)，台账中的初始行为保留作为整改依据。

每个 B 批次填写：涉及 D 编号、原版函数/提交、依赖裁剪、保留差异、正确性依据、预计热路径变化、实际测试命令与结果、未验证边界。没有 RDMA 硬件结果的批次只能标“纯逻辑验证通过/硬件待验”。

B1、B2 不以只删除 new/delete 或 pending 成员为完成标准；必须逐项证明池化字段重置、异步唯一所有者和失败排空闭合。B4 不以同步调用能返回为完成标准；必须验证响应/超时/失败竞争、迟到响应、done 释放用户对象。B6 不引入 health-check→Revive，但必须验证对端退出与本端关闭的收尾。

本报告完成的是源码差异审计和整改约束，不是性能等价证明，也不是运行时整改完成报告。
