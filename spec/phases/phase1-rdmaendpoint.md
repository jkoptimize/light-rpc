# Phase 1: FastRdmaEndpoint

> per-connection RDMA 端点：连接握手、流控、发送/接收、CQ 处理
>
> **传输层方案**：raw ibv_verbs + TCP 带外握手（非 rdma_cm），不链接 librdmacm
> **测试方案**：`g_skip_rdma_init` 开关，测试放 `test/unit/` 目录

---

## 目标

实现 per-connection 的 RDMA 封装类，承担以下职责：

1. **连接握手**：交换 HelloMessage，协商流控参数（sq_size、rq_size、block_size）
2. **RDMA 资源管理**：QP/CQ 创建、销毁，QP 状态迁移（RESET→INIT→RTR→RTS）
3. **数据发送**：`CutFromIOBufList` — 由 KeepWrite 线程调用，将 IOBuf 切入 ibv_sge 并 ibv_post_send
4. **CQ 处理**：`PollCq` / `HandleCompletion` — 处理 send/recv WC，更新流控窗口，投递 recv WR
5. **流控**：窗口变量管理，SQ 满时阻塞 KeepWrite 线程

**不在 Phase 1 范围内**（属于 Phase 4 FastChannel）：
- `WriteRequest` 结构体 + `_write_head` 无锁队列
- `StartWrite` 入队逻辑
- `KeepWrite` 线程
- `EnqueueSend` 接口

---

## 测试目标

- [ ] **HelloMessage 序列化/反序列化/校验**：握手消息正确处理
- [ ] **流控窗口初始化**：根据握手参数计算 `local_window_capacity`、`remote_window_capacity`，初值正确
- [ ] **IsWritable**：`sq_window_size_ > 0 && remote_rq_window_size_ > 0` 时返回 true
- [ ] **WaitForWritable**：窗口满时阻塞，`HandleCompletion` 更新窗口后唤醒
- [ ] **HandleCompletion (SEND WC)**：释放 sbuf，更新 `sq_window_size_`，必要时 notify
- [ ] **HandleCompletion (RECV WC)**：数据写入 rbuf，更新 `remote_rq_window_size_`，PostRecv，SendAck
- [ ] **SendAck / SendImm 触发条件**：new_rq_wrs 累积超过阈值且 sq_imm_window_size > 0 时触发
- [ ] **CutFromIOBufList**：将 IOBuf block 正确填入 ibv_sge，扣减窗口，调用 ibv_post_send

---

## 接口设计

```cpp
namespace fast {

class FastRdmaEndpoint {
public:
    FastRdmaEndpoint();
    ~FastRdmaEndpoint();

    // ============ 连接建立（握手）============
    int ProcessHandshakeAtClient(int tcp_fd);
    int ProcessHandshakeAtServer(int tcp_fd);

    // ============ QP 资源管理 ============
    int AllocateResources(uint16_t sq_size, uint16_t rq_size);
    int BringUpQp(uint16_t lid, ibv_gid gid, uint32_t remote_qpn);
    void DeallocateResources();

    // ============ 发送（KeepWrite 线程调用）============
    ssize_t CutFromIOBufList(IOBuf** from, size_t ndata);
    bool IsWritable() const;
    void WaitForWritable();

    // ============ 接收 & CQ（Poller 线程调用）============
    void PollCq();
    ssize_t HandleCompletion(ibv_wc& wc);
    int PostRecv(uint32_t num, bool zerocopy);

    // ============ 查询 ============
    ibv_qp* qp() const { return qp_; }
    int comp_channel_fd() const;

private:
    int SendAck(int num);
    int SendImm(uint32_t imm);
    int DoPostRecv(void* block, size_t block_size);

    // RDMA 资源
    ibv_qp* qp_ = nullptr;
    ibv_cq* send_cq_ = nullptr;
    ibv_cq* recv_cq_ = nullptr;
    ibv_comp_channel* comp_channel_ = nullptr;

    // 协商参数（握手后确定）
    uint16_t sq_size_{128};
    uint16_t rq_size_{128};
    uint32_t remote_recv_block_size_{0};
    int local_window_capacity_{0};
    int remote_window_capacity_{0};

    // 流控变量
    std::atomic<int> sq_window_size_{0};
    std::atomic<int> remote_rq_window_size_{0};
    int sq_imm_window_size_{3};
    std::atomic<int> new_rq_wrs_{0};

    // 缓冲环
    std::vector<IOBuf> sbuf_;        // 发送缓冲
    size_t sq_current_{0};           // sbuf 写指针
    size_t sq_sent_{0};              // sbuf 释放指针

    std::vector<IOBuf> rbuf_;        // 接收缓冲
    std::vector<void*> rbuf_data_;   // 接收 buffer 地址
    size_t rq_received_{0};          // rbuf 指针

    // 阻塞等待
    std::mutex send_mutex_;
    std::condition_variable send_cv_;

    // selective signaling 统计
    int send_counter_{0};
    int sq_unsignaled_{0};
    int unsolicited_{0};
    int accumulated_ack_{0};
};

} // namespace fast
```

---

## 关键子模块逻辑

### HelloMessage (40B，TCP 交换)

握手通过 TCP 连接交换，40 字节 HelloMessage，包含建立 RC 连接所需的全部信息：

```cpp
struct HelloMessage {
    char     magic[4] = {'R','D','M','A'};  // 4B
    uint16_t msg_len = 40;                  // 2B
    uint16_t hello_ver = 1;                 // 2B
    uint16_t impl_ver = 1;                  // 2B，0 = 降级 TCP
    uint32_t block_size = 8192;             // 4B
    uint16_t sq_size;                       // 2B
    uint16_t rq_size;                       // 2B
    uint16_t lid;                           // 2B，IB LID
    uint8_t  gid[16];                       // 16B，RoCE GID
    uint32_t qp_num;                        // 4B

    void Serialize(void* data) const;
    void Deserialize(const void* data);
};  // 40 bytes total

    void Serialize(void* data) const;
    void Deserialize(const void* data);
};

// 握手校验
bool HelloNegotiationValid(const HelloMessage& msg);
```

### 握手流程（TCP 带外）

握手在建立 RDMA QP 之前，通过已连接的 TCP 通道进行。

**客户端**：`AllocateResources()` (创建 CQ+QP) → 发送 `HelloMessage`(TCP) → 接收对端 `HelloMessage` → `BringUpQp()` (RESET→INIT→RTR→RTS) → 发送 ACK(4B)

**服务端**：收到 `HelloMessage`(TCP) → `AllocateResources()` → 发送 `HelloMessage`(TCP) → `BringUpQp()` → 收到 ACK(4B)

> **不支持 TCP 降级**：当前 Phase 1 不实现 TCP fallback，握手失败直接 CHECK 退出。

握手完成后：
```cpp
local_window_capacity  = std::min(sq_size_, remote_msg.rq_size) - 3;
remote_window_capacity = std::min(rq_size_, remote_msg.sq_size) - 3;
sq_window_size_ = local_window_capacity;
remote_rq_window_size_ = local_window_capacity;
sq_imm_window_size_ = 3;
remote_recv_block_size_ = remote_msg.block_size;
```

### CutFromIOBufList

参考 brpc `rdma_endpoint.cpp::CutFromIOBufList`，核心逻辑：

1. 循环遍历 `from[0..ndata-1]` 中的 IOBuf
2. 对每个 IOBuf，调用 `cut_into_sglist_and_iobuf` 切 block → sbuf，填充 `ibv_sge` 数组
3. 每批不超过 `MAX_SGE` 条、不超过 `remote_recv_block_size_` 字节
4. 构建 `ibv_send_wr`，设置 solicited/signaled 标志
5. `ibv_post_send`，更新 `sq_current_`，扣减窗口

```cpp
// IOBuf block → ibv_sge 的切分（辅助函数）
ssize_t cut_into_sglist_and_iobuf(IOBuf& src, ibv_sge* sglist,
    size_t* sge_index, IOBuf* holder, size_t max_sge, size_t max_len);
```

### HandleCompletion

```cpp
ssize_t HandleCompletion(ibv_wc& wc) {
    switch (wc.opcode) {
    case IBV_WC_SEND:
        if (wc.wr_id == 0) {
            // Pure ACK 完成，释放 IMM 槽位
            sq_imm_window_size_ += 1;
            SendAck(0);
            return 0;
        }
        // 批量 SEND 完成：释放 sbuf，恢复 SQ 窗口
        for (int i = 0; i < wc.wr_id; ++i) {
            sbuf_[sq_sent_++].clear();
            if (sq_sent_ == sbuf_.size()) sq_sent_ = 0;
        }
        sq_window_size_.fetch_add(wc.wr_id);
        send_cv_.notify_one();  // 唤醒 KeepWrite
        return 0;

    case IBV_WC_RECV:
        // 数据写入 rbuf
        if (wc.byte_len > 0) { /* rbuf → 上层 buffer */ }
        // 更新对端 RQ 窗口
        if (wc.wc_flags & IBV_WC_WITH_IMM && wc.imm_data > 0) {
            remote_rq_window_size_.fetch_add(wc.imm_data);
            send_cv_.notify_one();
        }
        PostRecv(1, true);
        if (wc.byte_len > 0) SendAck(1);
        return wc.byte_len;

    default:
        return -1;
    }
}
```

### 流控变量语义

| 变量 | 含义 | 增减时机 |
|------|------|---------|
| `sq_window_size_` | 本端 SQ 剩余槽位 | post_send 时 -1，send WC 时 +wr_id |
| `remote_rq_window_size_` | 对端 RQ 剩余（估计）| post_send 时 -1，recv WC 的 imm_data 时 +ack |
| `sq_imm_window_size_` | Pure ACK 专用槽位（3 个）| SendImm 时 -1，IMM WC 时 +1 |
| `new_rq_wrs_` | 累积待发送的 recv 信用 | PostRecv 时 +1，SendImm 时 exchange 到 imm_data |

### 阻塞等待

```cpp
void FastRdmaEndpoint::WaitForWritable() {
    std::unique_lock<std::mutex> lock(send_mutex_);
    send_cv_.wait(lock, [this] { return IsWritable(); });
}
```

---

## 测试用例

### 1. HelloMessage 序列化/反序列化

```cpp
TEST(FastRdmaEndpoint, HelloMessageSerialize) {
    HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size = 128;
    msg.rq_size = 128;

    uint8_t buf[40];
    msg.Serialize(buf);

    HelloMessage msg2;
    msg2.Deserialize(buf);

    EXPECT_EQ(msg2.block_size, 8192);
    EXPECT_EQ(msg2.sq_size, 128);
    EXPECT_EQ(msg2.rq_size, 128);
}
```

### 2. 握手协商校验

```cpp
TEST(FastRdmaEndpoint, HelloNegotiationValid) {
    HelloMessage msg;
    msg.hello_ver = 1;
    msg.impl_ver = 1;
    msg.block_size = 8192;
    msg.sq_size = 128;
    msg.rq_size = 128;
    EXPECT_TRUE(HelloNegotiationValid(msg));

    msg.impl_ver = 0;  // 降级 TCP
    EXPECT_FALSE(HelloNegotiationValid(msg));
}
```

### 3. 流控窗口初始化

```cpp
TEST(FastRdmaEndpoint, FlowControlInit) {
    // 模拟握手协商结果
    // 本端 sq=32, 对端 rq=32
    // local_window_capacity = min(32, 32) - 3 = 29
    FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    EXPECT_TRUE(ep.IsWritable());
}
```

### 4. WaitForWritable 阻塞与唤醒

```cpp
TEST(FastRdmaEndpoint, WaitForWritableBlockAndWake) {
    FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    // 耗尽窗口：post 29 次 send（模拟）
    for (int i = 0; i < 29; i++) ep.SimulateSendOne();

    EXPECT_FALSE(ep.IsWritable());

    std::atomic<bool> woken{false};
    std::thread waiter([&] {
        ep.WaitForWritable();
        woken = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(woken);

    // 模拟 send WC 完成 1 个
    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id = 1;
    ep.HandleCompletion(wc);

    waiter.join();
    EXPECT_TRUE(woken);
}
```

### 5. SendAck 触发

```cpp
TEST(FastRdmaEndpoint, SendAckTrigger) {
    FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    // new_rq_wrs = 5, 不触发（< remote_window_capacity / 2）
    EXPECT_EQ(ep.SendAck(5), 0);

    // new_rq_wrs 累积超过一半，触发 SendImm
    EXPECT_NE(ep.SendAck(15), 0);
}
```

### 6. HandleCompletion (SEND WC)

```cpp
TEST(FastRdmaEndpoint, HandleSendCompletion) {
    FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    // 消耗 3 个窗口
    ep.SimulateSendN(3);
    EXPECT_EQ(ep.sq_window_size(), 26);

    // 完成 3 个 SEND
    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id = 3;
    ep.HandleCompletion(wc);

    EXPECT_EQ(ep.sq_window_size(), 29);
}
```

---

## 实现文件

| 文件 | 说明 |
|------|------|
| `inc/fast_iobuf.h` / `inc/fast_iobuf_inl.h` | + `fetch1()`（public）|
| `src-common/fast_iobuf.cc` | + `IOBuf::fetch1()` 实现 |
| `inc/fast_rdma_endpoint.h` | 头文件（FastRdmaEndpoint + HelloMessage + RdmaIOBuf）|
| `src-endpoint/fast_rdma_endpoint.cc` | 实现（含 `g_skip_rdma_init`）|

## CMakeLists 更新

新增源文件：`src-endpoint/fast_rdma_endpoint.cc`
新增测试：`test/unit/test_rdma_endpoint.cc`

---

## 下一步

Phase 2: EventDispatcher
