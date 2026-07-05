# Spec: Control QP + Data QP 双 QP 消息分类架构

> 状态：设计完成
> 日期：2026-07-04

---

## 一、核心思想

per-connection 单 QP 拆分为 **Control QP + Data QP**（握手时一并分配）：

| QP | 用途 | 流控机制 |
|----|------|----------|
| Control QP | Inline(≤200B) + Medium(200B~2MB) + Large 控制信令 | Credit-based（现有，不变） |
| Data QP | Large 数据面(≥2MB) | 被动授权——接收方先 post recv，发送方再 RDMA WRITE，零 RNR |

---

## 二、统一帧格式

```
[total_len:4B BE][msg_type:4B BE][rpc_id:4B BE][...type-specific...]
```

### 消息类型

| msg_type | 值 | 含义 | 后续载荷 |
|----------|----|------|----------|
| MSG_NORMAL | 0 | Inline/Medium RPC 请求 | [meta_len:4B][MetaDataOfRequest][payload][attachment] |
| MSG_NOTIFY | 1 | Large 传输通知 | [data_total_len:4B]（16B 帧） |
| MSG_AUTHORITY | 2 | Large 授权回复 | [rkey:4B][remote_addr:8B]（24B 帧） |
| MSG_NORMAL_RESPONSE | 3 | RPC 响应 | [error_code:4B][attachment_size:4B][payload][attachment] |

### 关键原则

- **Control QP IMM 语义不变**：`imm_data` 恒为 `htonl(new_rq_wrs_)`，纯流控
- **msg_type 判断在业务层**：RdmaEndpoint 不感知消息类型
- **CutInputMessage 不变**：从 offset 0 读 total_len(BE) 切帧
- **协议对称**：Client 和 Server 均可发起 Large 传输（请求和响应）
- **单 Handler 模型**：无论数据来自 Control QP 还是 Data QP，业务层通过同一个 IOBuf 路径处理

---

## 三、Large 消息协议（请求+响应，完全对称）

### 3.1 请求方向（Client→Server）

```
Client (Control QP)                Server (Control QP)           Server (Data QP)
      |                                    |                          |
      |—1. MSG_NOTIFY → StartWrite         |                          |
      |                                    |—2. OnProcessRequest     |
      |                                    |   → LargeBlockAlloc     |
      |                                    |   → PostLargeWriteRecv——→|
      |←3. MSG_AUTHORITY ← StartWrite ———─|                          |
      |                                    |                          |
Client (Data QP)                                                      |
      |==4. RDMA_WRITE_WITH_IMM(请求帧)==→|                          |
      |   imm_data=rkey, wr_id=rpc_id      |←5. WC RECV_RDMA_WITH_IMM|
      |                                    |—6. wrap IOBuf           |
      |                                    |   → ProcessNewMessage   |
      |                                    |   → OnProcessRequest    |
      |←7. MSG_NORMAL_RESPONSE ────────── | (Control QP)            |
```

### 3.2 响应方向（Server→Client，对称）

```
Server (Control QP)                Client (Control QP)           Client (Data QP)
      |                                    |                          |
      |—1. MSG_NOTIFY → StartWrite         |                          |
      |   (响应帧已 StoreLargeFrame)        |—2. OnProcessResponse    |
      |                                    |   → LargeBlockAlloc     |
      |                                    |   → PostLargeWriteRecv——→|
      |←3. MSG_AUTHORITY ← StartWrite —───|                          |
      |                                    |                          |
Server (Data QP)                                                      |
      |==4. RDMA_WRITE_WITH_IMM(响应帧)==→|                          |
      |   imm_data=rkey, wr_id=rpc_id      |←5. WC RECV_RDMA_WITH_IMM|
      |                                    |—6. wrap IOBuf           |
      |                                    |   → ProcessNewMessage   |
      |                                    |   → OnProcessResponse   |
      |                                    |   → notify pending.cv   |
```

### 3.3 IOBuf 零拷贝模型（核心简化）

Data QP 到达的 LargeBlock 数据通过 `IOBuf::append_user_data_with_meta` 包装为 IOBuf（参考 brpc `iobuf.cpp:1121`），**不拷贝数据，不创建 LargeHandler**：

```cpp
// PollCq data_recv_cq inline:
IOBuf frame;
frame.append_user_data_with_meta(
    mr->addr,                                          // LargeBlock 数据指针
    wc.byte_len,                                       // 完整帧长度
    [](void* mr) { ReturnLargeBlock((ibv_mr*)mr); },   // 析构回调：归还 TLS cache
    0                                                   // meta 预留
);
ep->read_buf_.append(std::move(frame));
// 后续走统一的 ProcessNewMessage(read_buf_) → CutInputMessage → handler
```

**关键设计影响**：
- MessageDispatcher **无需** LargeHandler、ProcessNewMessage 重载、DispatchLarge
- 业务层 **无需** OnProcessLargeRequest / OnProcessLargeResponse 独立函数
- LargeBlock 生命周期由 IOBuf Block ref-count 自动管理，回调触发 `ReturnLargeBlock`
- 附件（attachment）零拷贝：业务层直接从 IOBuf 引用 LargeBlock 内存，无需序列化

---

## 四、MessageDispatcher（统一单 Handler）

```cpp
class MessageDispatcher {
public:
    using MessageHandler = std::function<int(IOBuf& frame, void* arg)>;

    void SetHandler(MessageHandler handler, void* arg);
    void SetMode(DispatcherMode mode);

    // 统一入口：从 IOBuf 切帧 → detached thread → handler
    int ProcessNewMessage(IOBuf& read_buf);
};
```

**与当前代码完全一致，零改动**。Data QP 数据通过 append_user_data_with_meta 附加到 `read_buf_`，`CutInputMessage` 读出 total_len → 切帧 → detached thread → handler。handler 无法也不需区分数据来源。

---

## 五、PollCq 4 CQ 轮询

### 5.1 轮询顺序：recv 优先于 send

```
Phase 1 — 所有 recv（驱动业务前进）:
  poll control recv_cq → HandleCompletion(wc) → read_buf_ 积累
  poll data_recv_cq:
    rkey = ntohl(wc.imm_data)
    mr = ep->pending_large_map_ 取出并 erase
    IOBuf frame;  frame.append_user_data_with_meta(mr->addr, wc.byte_len,
        [](void* mr) { ReturnLargeBlock((ibv_mr*)mr); }, 0);
    ep->read_buf_.append(std::move(frame));
    // 不 re-post recv

  ProcessNewMessage(read_buf_)  // 统一分发（Control + Data 帧）
  if (recv_cnt > 0) continue

Phase 2 — 所有 send（清理资源）:
  poll control send_cq → HandleCompletion(wc)
  poll data_send_cq:
    ReleaseLargeFrame(wc.wr_id)
    OnLargeTransferComplete()  // --active_large_transfers_ + 回调

  if (send_cnt > 0) continue → back to Phase 1

Phase 3 — re-arm 4 CQ + re-poll + MoreReadEvents → break or restart
```

### 5.2 Data recv/send CQ 均为 inline，无独立 Handle 函数

### 5.3 4 CQ 共享 comp_channel

`GetAndAckEvents` 扩展为 4 CQ （send_cq_ / recv_cq_ / data_send_cq_ / data_recv_cq_），4 个计数器，模式不变。

---

## 六、Large 传输流控（Data QP SQ 深度准入）

### 6.1 Counter 在 FastRdmaEndpoint

```cpp
// FastRdmaEndpoint
static constexpr int kMaxLargeTransfers = 8;
std::atomic<int> active_large_transfers_{0};

bool CanStartLargeTransfer() const;
void StartLargeTransfer();        // ++ (WaitForLargeWritable 通过后)
void OnLargeTransferComplete();   // -- (PollCq data_send_cq 中调用 + 回调)

std::function<void()> _large_done_cb;  // 业务层注册
```

### 6.2 ±生命周期

```
++ : MSG_NOTIFY 发送前（WaitForLargeWritable 通过后，业务层）
-- : Data QP send WC 返回时（PollCq → OnLargeTransferComplete → callback → large_cv_.notify_one）
```

RDMA RC 语义：send WC 返回 = 远端已回复 transport ACK，Data QP SQ 槽位已释放。两端（Client/Server）完全对称。

### 6.3 业务层

```cpp
// FastChannel / FastServer 同时注册：
ep->_large_done_cb = [this] { large_cv_.notify_one(); };

// 各自在 MSG_NOTIFY 发送前调用：
void WaitForLargeWritable() {
    std::unique_lock<std::mutex> lock(large_mutex_);
    large_cv_.wait(lock, [this] {
        return ep->CanStartLargeTransfer() || closed_;
    });
    if (!closed_) ep->StartLargeTransfer();
}
```

---

## 七、LargeBlock 缓存

| 文件 | 内容 |
|------|------|
| `inc/fast_large_block.h` | `ibv_mr* LargeBlockAlloc(size_t size)` + `void ReturnLargeBlock(ibv_mr* mr)` |
| `src-common/fast_large_block.cc` | TLS 缓存（best-fit 分配/插入，LRU 淘汰，ThreadExitHelper） |

`ibv_reg_mr(g_pd, buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE)` 用全局 `g_pd`。

`ReturnLargeBlock` 不直接调用——注册为 IOBuf meta callback，在 IOBuf 最后一个 BlockRef 析构时自动触发。

### 调用链

```
LargeBlockAlloc → MSG_AUTHORITY 返回 rkey+addr → Data QP RDMA WRITE →
  recv WC → append_user_data_with_meta(addr, len, [mr]{ReturnLargeBlock(mr)}, 0) →
  业务层处理 → IOBuf 析构 → callback → ReturnLargeBlock(mr) → TLS cache
```

所有 ReturnLargeBlock 由 IOBuf 自动驱动，业务层完全不感知。

---

## 八、CutSegFromIOBuf（独立函数，内部完成 RDMA WRITE post）

```cpp
// 从 IOBuf 提取 sge 数组并直接 post RDMA WRITE with IMM
// 参数：IOBuf 帧、远端 rkey/addr、本端 rkey（作为 imm_data）、rpc_id（作为 wr_id）
ssize_t FastRdmaEndpoint::CutSegFromIOBuf(IOBuf* buf,
                                           uint32_t remote_rkey,
                                           uint64_t remote_addr,
                                           uint32_t imm_rkey,
                                           uint32_t rpc_id) {
    ibv_sge sglist[MAX_SGE];
    ssize_t nsge = // 遍历 IOBuf block refs → 提取 addr/lkey/length 到 sge
    ibv_send_wr wr = {};
    wr.opcode              = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.imm_data            = htonl(imm_rkey);
    wr.wr_id               = rpc_id;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;
    wr.sg_list             = sglist;
    wr.num_sge             = nsge;
    ibv_send_wr* bad = nullptr;
    return ibv_post_send(data_qp_, &wr, &bad);
}
```

- 内部完成 post，不经过流控窗口
- 调用位置：业务层 OnProcessRequest/OnProcessResponse 中 MSG_AUTHORITY 分支
- 与 `CutFromIOBufList` 区别：opcode 不同、需要 `wr.rdma` 字段、不 cut 到 sbuf_、无流控

---

## 九、PostLargeWriteRecv（按需，零长 IMM recv）

```cpp
int FastRdmaEndpoint::PostLargeWriteRecv(ibv_mr* mr) {
    ibv_sge sge = {};
    sge.addr   = reinterpret_cast<uint64_t>(mr->addr);
    sge.length = 4;
    sge.lkey   = mr->lkey;   // 实际注册 MR 的 lkey（非 0，非 dummy）
    ibv_recv_wr wr = {};
    wr.sg_list = &sge;
    wr.num_sge = 1;
    ibv_recv_wr* bad = nullptr;
    return ibv_post_recv(data_qp_, &wr, &bad);
}
```

两端 MSG_NOTIFY 到达时各调用一次，**不 re-post**。

---

## 十、业务层分支逻辑

### 10.1 FastChannel::CallMethod

```
1. 序列化帧 [total_len][MSG_NORMAL][rpc_id][meta_len][meta][payload][attachment]
2. total_len 判断:
   - < msg_threshold:
       → StartWrite(Control QP) → cv.wait()
   - >= msg_threshold:
       a. pending.is_large = true
       b. WaitForLargeWritable()
       c. ep->StoreLargeFrame(rpc_id, std::move(frame))
       d. MSG_NOTIFY [total_len=16][MSG_NOTIFY][rpc_id][data_total_len]
          → StartWrite(Control QP)
       e. cv.wait()
```

### 10.2 FastChannel::OnProcessResponse（MessageDispatcher handler）

```
parse [total_len][msg_type][rpc_id]:

  MSG_NOTIFY:
    → mr = LargeBlockAlloc(data_total_len)
    → ep->PostLargeWriteRecv(mr)
    → MSG_AUTHORITY [total_len=24][MSG_AUTHORITY][rpc_id][rkey][addr]
      → StartWrite(Control QP)
    → ep->pending_large_map_[rkey] = mr
    → return 0

  MSG_AUTHORITY:
    → frame = ep->GetLargeFrame(rpc_id)
    → ep->CutSegFromIOBuf(frame, remote_rkey, remote_addr, msg.rkey, rpc_id)
    → return 0  // frame 由 PollCq data_send_cq ReleaseLargeFrame 释放

  MSG_NORMAL_RESPONSE:
    → 现有逻辑：parse error_code → attachment_size → payload → attachment
    → if pending->is_large: pending.is_large 仅标记，不操作 counter (counter 在 PollCq)
    → notify pending.cv
```

### 10.3 FastServer::OnProcessRequest（MessageDispatcher handler）

```
parse [total_len][msg_type][rpc_id]:

  MSG_NORMAL:
    → 现有逻辑：parse meta → dispatch service → ReturnRPCResponse
    → （LargeBlock 数据到达时也是 MSG_NORMAL——IOBuf 中 meta/payload/attachment 统一处理）

  MSG_NOTIFY:
    → mr = LargeBlockAlloc(data_total_len)
    → ep->PostLargeWriteRecv(mr)
    → MSG_AUTHORITY → StartWrite(Control QP)
    → ep->pending_large_map_[mr->rkey] = mr
    → return 0

  MSG_AUTHORITY:
    → frame = ep->GetLargeFrame(rpc_id)
    → ep->CutSegFromIOBuf(frame, remote_rkey, remote_addr, msg.rkey, rpc_id)
    → return 0
```

> **注意**：OnProcessRequest 的 MSG_NORMAL 分支同时处理两种来源的请求帧——Control QP SEND 和 Data QP RDMA WRITE。Data QP 到达的数据通过 `append_user_data_with_meta` 包装为 IOBuf → read_buf_ → CutInputMessage → handler，handler 收到的 IOBuf 中包含 meta/payload/attachment，其中 attachment 直接引用 LargeBlock 内存（零拷贝）。

### 10.4 FastServer::ReturnRPCResponse（Large 响应分支）

```
响应 total_len >= msg_threshold:
  a. WaitForLargeWritable()
  b. ep->StoreLargeFrame(rpc_id, std::move(response_frame))
  c. MSG_NOTIFY → StartWrite(Control QP)
  // MSG_AUTHORITY 到达时 OnProcessRequest 触发 CutSegFromIOBuf
```

---

## 十一、Handler 注册

```cpp
// 服务端（OnServerAccept 中）
ep->msg_dispatcher().SetHandler(FastServer::OnProcessRequest, ep);

// 客户端（FastChannel 构造中）
endpoint_->msg_dispatcher().SetHandler(FastChannel::OnProcessResponse, this);
```

**单 handler，无 LargeHandler**。与当前代码一致，无需改动。

---

## 十二、Data QP 握手

### 12.1 双 QP 分配（AllocateResources 重构）

```cpp
// 公共 helper
static ibv_cq* CreateCq(int size, ibv_comp_channel* ch);
static ibv_qp* CreateQp(ibv_cq* scq, ibv_cq* rcq, int swr, int rwr, int sge_s, int sge_r);
```

```
AllocateResources:
  1. comp_channel_ = ibv_create_comp_channel(g_ctx)       // 4 CQ 共享
  2. send_cq_      = CreateCq(sq_size_,      comp_channel_)
  3. recv_cq_      = CreateCq(rq_size_,      comp_channel_)
  4. data_send_cq_ = CreateCq(kDataQpDepth,  comp_channel_)
  5. data_recv_cq_ = CreateCq(kDataQpDepth,  comp_channel_)
  6. qp_      = CreateQp(send_cq_, recv_cq_, sq_size_, rq_size_, g_rdma_max_sge, 1)
  7. data_qp_ = CreateQp(data_send_cq_, data_recv_cq_, kDataQpDepth, kDataQpDepth, g_rdma_max_sge, 1)
  8. 注册 comp_channel fd 到 EventDispatcher
```

### 12.2 双 QP BringUp

| 参数 | Control QP | Data QP |
|------|-----------|---------|
| qp_access_flags | 0 | `IBV_ACCESS_REMOTE_WRITE` |
| max_send_wr / max_recv_wr | sq_size_ / rq_size_ (128) | 8 / 8 |
| sq_psn / rq_psn | 0 / 0 | 0 / 0（独立 PSN 空间） |
| PostRecv | 全量 | 不 post（按需 PostLargeWriteRecv） |
| rnr_retry | 7 | 0 |

### 12.3 HelloMessage 扩展

```cpp
uint32_t data_qp_num = 0;   // +4B → kMsgLen = 44
```

---

## 十三、新增/变更的数据结构

### FastRdmaEndpoint

```cpp
// — Data QP 资源 —
ibv_qp* data_qp_       = nullptr;
ibv_cq* data_send_cq_  = nullptr;
ibv_cq* data_recv_cq_  = nullptr;
int     data_send_cq_events{0};
int     data_recv_cq_events{0};
static constexpr int kDataQpDepth = 8;
static constexpr int kMaxLargeTransfers = 8;

// — Large 传输流控（counter 在 endpoint，回调桥接业务层）—
std::atomic<int> active_large_transfers_{0};
bool CanStartLargeTransfer() const;
void StartLargeTransfer();
void OnLargeTransferComplete();
std::function<void()> _large_done_cb;

// — Large 帧存储（两端共用，key=rpc_id）—
std::mutex large_frame_mutex_;
std::unordered_map<uint32_t, IOBuf> pending_large_frames_;
void StoreLargeFrame(uint32_t rpc_id, IOBuf&& frame);
IOBuf* GetLargeFrame(uint32_t rpc_id);
void ReleaseLargeFrame(uint32_t rpc_id);

// — LargeBlock 跟踪（两端共用，key=rkey，直接存 ibv_mr*）—
std::mutex pending_large_mutex_;
std::unordered_map<uint32_t, ibv_mr*> pending_large_map_;

// — 工具函数 —
int PostLargeWriteRecv(ibv_mr* mr);
ssize_t CutSegFromIOBuf(IOBuf* buf, uint32_t remote_rkey, uint64_t remote_addr,
                         uint32_t imm_rkey, uint32_t rpc_id);
```

### FastChannel / FastServer（各自身份）

```cpp
// 流控等待
std::mutex              large_mutex_;
std::condition_variable large_cv_;
bool                    closed_{false};

// PendingRequest 新增字段（FastChannel 侧）
bool is_large = false;  // 仅标记，不在业务层操作 counter
```

### IOBuf 新增

```cpp
// 参考 brpc iobuf.cpp:1121 — 零拷贝包装外部内存
int append_user_data_with_meta(void* data, size_t size,
                                std::function<void(void*)> deleter,
                                uint64_t meta);
```

---

## 十四、rkey 与 imm_data 说明

| 字段 | 层面 | 作用 |
|------|------|------|
| `wr.wr.rdma.rkey` | RDMA 硬件 | NIC 校验对端内存访问权限 |
| `wr.imm_data` | 应用层 | 随 RDMA WRITE 到对端 CQE，O(1) 在 `pending_large_map_` 中查 LargeBlock |
| `mr->rkey`（ibv_reg_mr 返回值） | 全局唯一 | 同时在两者中使用 |
| `wr.wr_id` | 应用层 | = rpc_id，PollCq 中 `ReleaseLargeFrame` |

---

## 十五、关键设计对比：新旧方案

| 维度 | 原单 QP | 双 QP（spec 最终版） |
|------|---------|---------------------|
| RNR 风险 | credit 流控，rnr_retry=0 | Data QP 授权模型，零 RNR |
| 分发路径 | 单 handler（CutInputMessage + read_buf_） | 单 handler（不变，Data QP 数据通过 append_user_data 融入 read_buf_） |
| Large Block 生命周期 | LargeBlockAlloc → 手动 ReturnLargeBlock | IOBuf meta callback 自动驱动 |
| 附件性能 | 必须 copy 到新 IOBuf（脱离原 buffer） | 零拷贝直接引用 LargeBlock 内存 |
| MessageDispatcher | 无需改动 | 无需改动 |
| 流控 | 双窗口 + Pure ACK | Control QP 不变，Data QP 深度准入（8） |
