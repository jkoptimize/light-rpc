# Phase 1: FastRdmaEndpoint

> per-connection RDMA 端点。raw ibv_verbs + TCP 带外握手，不依赖 librdmacm。

---

## 目标

实现完整的 per-connection RDMA 封装类：

1. **连接握手**：TCP 交换 HelloMessage，协商流控参数
2. **RDMA 资源管理**：QP/CQ/comp_channel 创建、销毁，QP 状态迁移
3. **发送**：`CutFromIOBufList` — 将 IOBuf 切入 ibv_sge 并 ibv_post_send
4. **接收 & CQ**：`PollCq` / `HandleCompletion` — 处理 WC，流控窗口，投递 recv WR
5. **流控**：窗口变量管理，SQ 满时阻塞 KeepWrite

**不在 Phase 1 范围内**（属于 Phase 4 FastChannel）：
- `WriteRequest` 结构体 + `_write_head` 无锁队列
- `StartWrite` 入队逻辑
- `KeepWrite` 线程

---

## 全局资源（文件作用域）

`GlobalInitialize()` 调用一次，初始化全局 RDMA 资源：

```cpp
// 文件作用域全局变量 (fast_rdma_endpoint.cc)
static ibv_context*    g_ctx = nullptr;
static ibv_pd*         g_pd  = nullptr;
static ibv_gid         g_gid = {};     // RoCE GID
static uint16_t        g_lid = 0;      // IB LID
static int             g_rdma_max_sge = 32;
static uint32_t        g_rdma_recv_block_size = 0;
static uint32_t        g_rdma_zerocopy_min_size = 512;

void FastRdmaEndpoint::GlobalInitialize() {
    ibv_device** devs = ibv_get_device_list(nullptr);
    CHECK(devs && devs[0]);
    g_ctx = ibv_open_device(devs[0]);
    CHECK(g_ctx);
    ibv_free_device_list(devs);

    g_pd = ibv_alloc_pd(g_ctx);
    CHECK(g_pd);

    ibv_device_attr attr;
    ibv_query_device(g_ctx, &attr);
    g_rdma_max_sge = attr.max_sge;

    ibv_port_attr port_attr;
    ibv_query_port(g_ctx, 1, &port_attr);
    g_lid = port_attr.lid;
    ibv_query_gid(g_ctx, 1, 0, &g_gid);  // RoCE GID index 0

    g_rdma_recv_block_size = GetBlockSize(0) - RdmaIOBuf::IOBUF_BLOCK_HEADER_LEN;
}
```

---

## 接口设计

```cpp
namespace fast {

struct HelloMessage { /* 40B, TCP 交换 */ };

bool HelloNegotiationValid(const HelloMessage& msg);

class RdmaIOBuf : public IOBuf {
    friend class FastRdmaEndpoint;
public:
    static const size_t IOBUF_BLOCK_HEADER_LEN = 32;
private:
    ssize_t cut_into_sglist_and_iobuf(ibv_sge* sglist, size_t* sge_index,
                                       IOBuf* to, size_t max_sge, size_t max_len);
};

class FastRdmaEndpoint {
public:
    FastRdmaEndpoint();
    ~FastRdmaEndpoint();

    // ---- Global init ----
    static void GlobalInitialize();

    // ---- Handshake (static, ep passed as arg) ----
    static int ProcessHandshakeAtClient(FastRdmaEndpoint* ep, int tcp_fd);
    static int ProcessHandshakeAtServer(FastRdmaEndpoint* ep, int tcp_fd);

    // ---- QP resource management ----
    int AllocateResources();   // uses sq_size_ / rq_size_
    int BringUpQp(uint16_t lid, ibv_gid gid, uint32_t remote_qpn);
    void DeallocateResources();

    // ---- Send (called by KeepWrite thread) ----
    ssize_t CutFromIOBufList(IOBuf** from, size_t ndata);
    bool IsWritable() const;
    void WaitForWritable();

    // ---- Recv & CQ (called by Poller thread) ----
    static void PollCq(FastRdmaEndpoint* ep);
    ssize_t HandleCompletion(ibv_wc& wc);
    int PostRecv(uint32_t num, bool zerocopy);

    // ---- Query ----
    ibv_qp* qp() const { return qp_; }
    int comp_channel_fd() const;

    // ---- Test helpers (unit tests only, not for production) ----
    int sq_window_size() const { ... }
    int remote_rq_window_size() const { ... }
    int new_rq_wrs() const { ... }
    void SetNegotiatedParams(...);
    void SimulateSendOne();
    void SimulateSendN(int n);
    int  TestSendAck(int num) { return SendAck(num); }

private:
    int SendAck(int num);
    int SendImm(uint32_t imm);
    int DoPostRecv(void* block, size_t block_size);
    static int ReadFromFd(int fd, void* data, size_t len);
    static int WriteToFd(int fd, const void* data, size_t len);

    // ---- RDMA resources ----
    ibv_qp*            qp_ = nullptr;
    ibv_cq*            send_cq_ = nullptr;
    ibv_cq*            recv_cq_ = nullptr;
    ibv_comp_channel*  comp_channel_ = nullptr;

    // ---- Negotiated params ----
    uint16_t sq_size_{128};
    uint16_t rq_size_{128};
    uint32_t remote_recv_block_size_{0};
    int      local_window_capacity_{0};
    int      remote_window_capacity_{0};

    // ---- Flow control ----
    std::atomic<int> sq_window_size_{0};
    std::atomic<int> remote_rq_window_size_{0};
    int              sq_imm_window_size_{3};
    std::atomic<int> new_rq_wrs_{0};

    // ---- Buffer rings ----
    std::vector<IOBuf>  sbuf_;
    size_t              sq_current_{0};
    size_t              sq_sent_{0};
    std::vector<IOBuf>  rbuf_;
    std::vector<void*>  rbuf_data_;
    size_t              rq_received_{0};
    IOBuf               read_buf_;      // 接收数据累积

    // ---- Blocking wait ----
    std::mutex              send_mutex_;
    std::condition_variable send_cv_;

    // ---- Selective signaling ----
    int send_counter_{0};
    int sq_unsignaled_{0};
    int unsolicited_{0};
    int accumulated_ack_{0};
};

} // namespace fast
```

---

## 关键子模块逻辑

### HelloMessage (40B, TCP 交换)

```cpp
struct HelloMessage {
    char     magic[4]    = {'R','D','M','A'};
    uint16_t msg_len     = 40;
    uint16_t hello_ver   = 1;
    uint16_t impl_ver    = 1;       // 0 = 降级 TCP
    uint32_t block_size  = 8192;
    uint16_t sq_size;
    uint16_t rq_size;
    uint16_t lid;
    uint8_t  gid[16];
    uint32_t qp_num;

    void Serialize(void* data) const;
    void Deserialize(const void* data);
};
```

### 握手流程

**客户端**：`AllocateResources()` → 发 `HelloMessage`(TCP) → 收对端 `HelloMessage` → `BringUpQp()` → 发 ACK

**服务端**：收 `HelloMessage`(TCP) → 设协商参数 → `AllocateResources()` → `BringUpQp()` → 发 `HelloMessage`(TCP) → 收 ACK

握手完成后：
```cpp
local_window_capacity  = std::min(sq_size_, remote.rq_size) - 3;
remote_window_capacity = std::min(rq_size_, remote.sq_size) - 3;
sq_window_size_  = local_window_capacity;
remote_rq_window_size_ = local_window_capacity;
sq_imm_window_size_ = 3;
remote_recv_block_size_ = remote.block_size;
```

### AllocateResources

```cpp
int FastRdmaEndpoint::AllocateResources() {
    comp_channel_ = ibv_create_comp_channel(g_ctx);
    CHECK(comp_channel_);

    send_cq_ = ibv_create_cq(g_ctx, sq_size_, nullptr, comp_channel_, 0);
    recv_cq_ = ibv_create_cq(g_ctx, rq_size_, nullptr, comp_channel_, 0);
    CHECK(send_cq_ && recv_cq_);

    ibv_qp_init_attr qp_attr = {};
    qp_attr.send_cq = send_cq_;
    qp_attr.recv_cq = recv_cq_;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = sq_size_;
    qp_attr.cap.max_recv_wr  = rq_size_;
    qp_attr.cap.max_send_sge = g_rdma_max_sge;
    qp_attr.cap.max_recv_sge = 1;
    qp_ = ibv_create_qp(g_pd, &qp_attr);
    CHECK(qp_);

    sbuf_.resize(sq_size_ - 3);
    rbuf_.resize(rq_size_);
    rbuf_data_.resize(rq_size_, nullptr);

    ibv_req_notify_cq(send_cq_, 0);  // any completion
    ibv_req_notify_cq(recv_cq_, 1);  // solicited only

    return 0;
}
```

### BringUpQp (RESET→INIT→RTR→RTS)

```cpp
int FastRdmaEndpoint::BringUpQp(uint16_t lid, ibv_gid gid, uint32_t remote_qpn) {
    ibv_qp_attr attr = {};

    // RESET → INIT
    attr.qp_state   = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num   = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    CHECK(ibv_modify_qp(qp_, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) == 0);

    // Post recv WRs
    CHECK(PostRecv(rq_size_, false) == 0);

    // INIT → RTR
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = IBV_MTU_1024;
    attr.dest_qp_num        = remote_qpn;
    attr.rq_psn             = 0;
    attr.max_dest_rd_atomic = 0;
    attr.min_rnr_timer      = 0;
    attr.ah_attr.dlid       = lid;
    attr.ah_attr.sl         = 0;
    attr.ah_attr.is_global  = 1;
    attr.ah_attr.port_num   = 1;
    attr.ah_attr.grh.dgid   = gid;
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit  = 16;
    CHECK(ibv_modify_qp(qp_, &attr, IBV_QP_STATE | IBV_QP_PATH_MTU |
                        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                        IBV_QP_AV) == 0);

    // RTR → RTS
    memset(&attr, 0, sizeof(attr));
    attr.qp_state     = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 0;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 0;
    CHECK(ibv_modify_qp(qp_, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
                        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                        IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) == 0);
    return 0;
}
```

### DeallocateResources

```cpp
void FastRdmaEndpoint::DeallocateResources() {
    sbuf_.clear(); rbuf_.clear(); rbuf_data_.clear();
    if (qp_)      { ibv_destroy_qp(qp_);            qp_ = nullptr; }
    if (send_cq_) { ibv_destroy_cq(send_cq_);        send_cq_ = nullptr; }
    if (recv_cq_) { ibv_destroy_cq(recv_cq_);        recv_cq_ = nullptr; }
    if (comp_channel_) { ibv_destroy_comp_channel(comp_channel_); comp_channel_ = nullptr; }
}
```

### PostRecv / DoPostRecv

```cpp
int FastRdmaEndpoint::PostRecv(uint32_t num, bool zerocopy) {
    while (num-- > 0) {
        if (zerocopy) {
            rbuf_[rq_received_].clear();
            IOBufAsZeroCopyOutputStream os(&rbuf_[rq_received_],
                g_rdma_recv_block_size + RdmaIOBuf::IOBUF_BLOCK_HEADER_LEN);
            int size = 0;
            if (!os.Next(&rbuf_data_[rq_received_], &size)) return -1;
        }
        if (DoPostRecv(rbuf_data_[rq_received_], g_rdma_recv_block_size) < 0) {
            rbuf_[rq_received_].clear();
            return -1;
        }
        ++rq_received_;
        if (rq_received_ == rbuf_.size()) rq_received_ = 0;
    }
    return 0;
}

int FastRdmaEndpoint::DoPostRecv(void* block, size_t block_size) {
    ibv_recv_wr wr = {};
    ibv_sge sge = {};
    sge.addr  = reinterpret_cast<uint64_t>(block);
    sge.length = static_cast<uint32_t>(block_size);
    sge.lkey   = GetRegionId(block);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    ibv_recv_wr* bad = nullptr;
    return ibv_post_recv(qp_, &wr, &bad);
}
```

### SendImm

```cpp
int FastRdmaEndpoint::SendImm(uint32_t imm) {
    if (imm == 0) return 0;
    ibv_send_wr wr = {};
    wr.opcode    = IBV_WR_SEND_WITH_IMM;
    wr.imm_data  = htonl(imm);
    wr.send_flags = IBV_SEND_SOLICITED | IBV_SEND_SIGNALED;
    wr.wr_id     = 0;  // marks IMM completion
    ibv_send_wr* bad = nullptr;
    int ret = ibv_post_send(qp_, &wr, &bad);
    if (ret != 0) return -1;
    sq_imm_window_size_ -= 1;
    return 0;
}
```

### CutFromIOBufList（核心发送路径）

```cpp
ssize_t FastRdmaEndpoint::CutFromIOBufList(IOBuf** from, size_t ndata) {
    uint32_t remote_rq_wnd = remote_rq_window_size_.load(std::memory_order_relaxed);
    uint32_t sq_wnd        = sq_window_size_.load(std::memory_order_relaxed);

    size_t total_len = 0, current = 0;
    ibv_send_wr wr;
    ibv_sge sglist[g_rdma_max_sge];

    while (current < ndata) {
        if (remote_rq_wnd == 0 || sq_wnd == 0) {
            if (total_len > 0) break;
            errno = EAGAIN;
            return -1;
        }

        IOBuf* to = &sbuf_[sq_current_];
        size_t this_len = 0;
        memset(&wr, 0, sizeof(wr));
        wr.sg_list = sglist;
        wr.opcode  = IBV_WR_SEND_WITH_IMM;

        // Cut IOBufs into sge array
        size_t sge_index = 0;
        while (sge_index < (uint32_t)g_rdma_max_sge &&
               this_len < remote_recv_block_size_) {
            if (from[current]->empty()) {
                if (++current == ndata) break;
                continue;
            }
            RdmaIOBuf* rio = static_cast<RdmaIOBuf*>(from[current]);
            ssize_t len = rio->cut_into_sglist_and_iobuf(
                sglist, &sge_index, to, g_rdma_max_sge,
                remote_recv_block_size_ - this_len);
            if (len < 0) return -1;
            this_len += len;
            total_len += len;
        }
        if (this_len == 0) continue;

        wr.num_sge = sge_index;

        // IMM data: carry recv credits
        uint32_t imm = new_rq_wrs_.exchange(0, std::memory_order_relaxed);
        wr.imm_data = htonl(imm);

        // Solicited heuristic (control peer CQE rate)
        bool solicited = false;
        if (remote_rq_wnd == 1 || sq_wnd == 1 || current + 1 >= ndata) {
            solicited = true;
        } else if (unsolicited_ > local_window_capacity_ / 4) {
            solicited = true;
        } else if (accumulated_ack_ > remote_window_capacity_ / 4) {
            solicited = true;
        } else {
            ++unsolicited_;
            accumulated_ack_ += imm;
        }
        if (solicited) {
            wr.send_flags |= IBV_SEND_SOLICITED;
            unsolicited_ = 0;
            accumulated_ack_ = 0;
        }

        // Selective signaling
        if (++sq_unsignaled_ >= local_window_capacity_ / 4) {
            wr.send_flags |= IBV_SEND_SIGNALED;
            wr.wr_id = sq_unsignaled_;
            sq_unsignaled_ = 0;
        }

        ibv_send_wr* bad = nullptr;
        if (ibv_post_send(qp_, &wr, &bad) != 0) return -1;

        if (++sq_current_ == sbuf_.size()) sq_current_ = 0;
        remote_rq_wnd = remote_rq_window_size_.fetch_sub(1) - 1;
        sq_wnd        = sq_window_size_.fetch_sub(1) - 1;
    }
    return static_cast<ssize_t>(total_len);
}
```

### RdmaIOBuf::cut_into_sglist_and_iobuf

```cpp
ssize_t RdmaIOBuf::cut_into_sglist_and_iobuf(
        ibv_sge* sglist, size_t* sge_index,
        IOBuf* to, size_t max_sge, size_t max_len) {
    size_t len = 0;
    while (*sge_index < max_sge) {
        if (len >= max_len || _ref_num() == 0) break;
        const BlockRef& r    = _ref_at(0);
        const void*    start = fetch1();
        uint32_t lkey = GetRegionId(const_cast<void*>(start));
        if (lkey == 0) return -1;
        size_t n = r.length;
        if (len + n > max_len) n = max_len - len;
        size_t i = *sge_index;
        sglist[i].addr   = reinterpret_cast<uint64_t>(start);
        sglist[i].length = static_cast<uint32_t>(n);
        sglist[i].lkey   = lkey;
        cutn(to, n);
        len += n;
        ++(*sge_index);
    }
    return static_cast<ssize_t>(len);
}
```

### HandleCompletion

```cpp
ssize_t FastRdmaEndpoint::HandleCompletion(ibv_wc& wc) {
    bool zerocopy = true;
    switch (wc.opcode) {
    case IBV_WC_SEND:
        if (wc.wr_id == 0) {  // Pure ACK / IMM
            sq_imm_window_size_ += 1;
            SendAck(0);
            return 0;
        }
        for (uint16_t i = 0; i < wc.wr_id; ++i) {
            sbuf_[sq_sent_].clear();
            if (++sq_sent_ == sbuf_.size()) sq_sent_ = 0;
        }
        sq_window_size_.fetch_add(wc.wr_id, std::memory_order_relaxed);
        if (remote_rq_window_size_.load() >= local_window_capacity_ / 8) {
            send_cv_.notify_all();
        }
        return 0;

    case IBV_WC_RECV:
        if (wc.byte_len > 0) {
            if (wc.byte_len < g_rdma_zerocopy_min_size) zerocopy = false;
            if (zerocopy) rbuf_[rq_received_].cutn(&read_buf_, wc.byte_len);
            else          read_buf_.append(rbuf_data_[rq_received_], wc.byte_len);
        }
        if ((wc.wc_flags & IBV_WC_WITH_IMM) && wc.imm_data > 0) {
            uint32_t acks = ntohl(wc.imm_data);
            uint32_t prev = remote_rq_window_size_.fetch_add(acks);
            if (sq_window_size_.load() > 0 &&
                (prev >= local_window_capacity_ / 8 || acks >= local_window_capacity_ / 8)) {
                send_cv_.notify_all();
            }
        }
        if (PostRecv(1, zerocopy) < 0) return -1;
        if (wc.byte_len > 0) SendAck(1);
        return static_cast<ssize_t>(wc.byte_len);

    default:
        return -1;
    }
}
```

### PollCq

```cpp
void FastRdmaEndpoint::PollCq(FastRdmaEndpoint* ep) {
    // Phase 2 (EventDispatcher) 集成后实现：
    // ibv_poll_cq loop → ep->HandleCompletion for each WC
    // re-arm notification via ibv_req_notify_cq
}
```

### 流控变量

| 变量 | 含义 | 增减时机 |
|------|------|---------|
| `sq_window_size_` | 本端 SQ 剩余槽位 | post_send -1, send WC +wr_id |
| `remote_rq_window_size_` | 对端 RQ 剩余（估计）| post_send -1, recv WC imm_data +acks |
| `sq_imm_window_size_` | Pure ACK 专用槽位（3）| SendImm -1, IMM WC +1 |
| `new_rq_wrs_` | 累积待发 recv 信用 | PostRecv +1, post_send imm → exchange to 0 |

---

## 单元测试规则

- **纯逻辑可测**：HelloMessage 序列化/校验、流控窗口初始化/计算、HandleCompletion SEND WC 处理、WaitForWritable、SendAck 阈值
- **含 RDMA 接口（ibv_post_send / ibv_poll_cq / ibv_post_recv / ibv_modify_qp）的场景不写单测**
- 测试辅助方法（`SetNegotiatedParams` / `SimulateSendOne` / `TestSendAck`）注明 "For unit tests only"

---

## 实现文件

| 文件 | 说明 |
|------|------|
| `inc/fast_iobuf.h` / `src-common/fast_iobuf.cc` | + `fetch1()` |
| `inc/fast_rdma_endpoint.h` | FastRdmaEndpoint + HelloMessage + RdmaIOBuf |
| `src-endpoint/fast_rdma_endpoint.cc` | 完整实现（含全局变量 `g_ctx`/`g_pd`/`g_gid`/`g_lid`）|

## CMakeLists 更新

新增源文件：`src-endpoint/fast_rdma_endpoint.cc`
新增测试：`test/unit/test_rdma_endpoint.cc`

---

## 下一步

Phase 2: EventDispatcher
