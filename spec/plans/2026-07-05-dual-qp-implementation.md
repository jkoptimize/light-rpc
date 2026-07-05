# Dual QP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 per-connection 单 QP 拆分为 Control QP + Data QP，Large 消息走 Data QP RDMA WRITE 零拷贝路径。

**Architecture:** FastRdmaEndpoint 新增 Data QP 资源 + 4 CQ PollCq，业务层 FastChannel/FastServer 新增 Large 路径三分支（MSG_NOTIFY / MSG_AUTHORITY / MSG_NORMAL），LargeBlock 通过 IOBuf append_user_data_with_meta 零拷贝融入现有 MessageDispatcher。

**Tech Stack:** C++17, libibverbs, Protocol Buffers, gtest

## Global Constraints

- RDMA verbs 使用必须遵循 IB verbs 标准规范（见 CLAUDE.md）
- 含 ibv_* 接口的场景不写单元测试
- 纯逻辑可写单元测试（IOBuf、HelloMessage 序列化、流控 counter）
- 每次修改后编译通过：`cd build && make`
- 2 空格缩进，成员变量 `_` 后缀

---

## File Structure

| 文件 | 操作 | 职责 |
|------|------|------|
| `inc/fast_define.h` | 修改 | msg_type 枚举常量 |
| `inc/fast_iobuf_inl.h` | 修改 | `IOBUF_BLOCK_FLAGS_USER_DATA` flag, `dec_ref` 分支, `append_user_data_with_meta` 声明 |
| `src-common/fast_iobuf.cc` | 修改 | `append_user_data_with_meta` 实现 |
| `inc/fast_large_block.h` | **新建** | `LargeBlockAlloc` / `ReturnLargeBlock` 声明 |
| `src-common/fast_large_block.cc` | **新建** | TLS 缓存实现 |
| `inc/fast_rdma_endpoint.h` | 修改 | Data QP 字段, HelloMessage 扩展, 流控, pending maps, helpers |
| `src-endpoint/fast_rdma_endpoint.cc` | 修改 | AllocateResources 重构, BringUpQp(Data QP), PollCq 4 CQ, PostLargeWriteRecv, CutSegFromIOBuf |
| `inc/fast_channel.h` | 修改 | `WaitForLargeWritable`, `large_cv_`, `closed_` |
| `src-endpoint/fast_channel.cc` | 修改 | CallMethod Large 路径, OnProcessResponse 三分支 |
| `inc/fast_server.h` | 修改 | `WaitForLargeWritable`, `large_cv_`, `closed_` |
| `src-endpoint/fast_server.cc` | 修改 | OnProcessRequest 三分支, ReturnRPCResponse Large 分支 |
| `proto/fast_impl.proto` | 修改 | `ResponseHead.msg_type` 字段 |
| `CMakeLists.txt` | 修改 | 新增 fast_large_block 相关文件 |

**不变的文件**：`inc/message_dispatcher.h`、`src-endpoint/message_dispatcher.cc`、`inc/event_dispatcher.h`、`src-endpoint/event_dispatcher.cc`

---

### Task 1: msg_type 枚举 + 帧格式常量

**Files:**
- Modify: `inc/fast_define.h:28-32`

**Interfaces:**
- Produces: `enum MessageType { MSG_NORMAL=0, MSG_NOTIFY=1, MSG_AUTHORITY=2, MSG_NORMAL_RESPONSE=3 }`, fixed frame header sizes

**变更内容：**

当前 `fast_define.h` 中只有 `FAST_SmallMessage=1, FAST_NotifyMessage=2`。替换为完整 msg_type：

```cpp
// inc/fast_define.h — 替换现有 MessageType 枚举
enum MessageType : uint32_t {
    MSG_NORMAL           = 0,  // Inline/Medium RPC 请求
    MSG_NOTIFY           = 1,  // Large 传输通知
    MSG_AUTHORITY        = 2,  // Large 授权回复
    MSG_NORMAL_RESPONSE  = 3,  // RPC 响应
};

// 帧头固定大小
constexpr uint32_t kFrameHeaderBytes = 12;         // total_len + msg_type + rpc_id
constexpr uint32_t kNotifyFrameBytes  = 16;         // 12 + data_total_len (MSG_NOTIFY 帧)
constexpr uint32_t kAuthFrameBytes    = 24;         // 12 + rkey + remote_addr (MSG_AUTHORITY 帧)
constexpr uint32_t kRespHeaderBytes   = 16;         // 12B frame header + 4B error_code
```

- [ ] **Step 1: 修改 `inc/fast_define.h`** — 替换 MessageType 枚举，添加帧格式常量

- [ ] **Step 2: 编译验证**

```bash
cd build && make 2>&1 | head -30
```

Expected: 编译通过（仅常量变更，无调用方需同步修改）。

> 注：旧 `FAST_SmallMessage` / `FAST_NotifyMessage` 引用需在后续 Task 中逐步替换。

- [ ] **Step 3: Commit**

---

### Task 2: IOBuf append_user_data_with_meta + Block user data flag

**Files:**
- Modify: `inc/fast_iobuf_inl.h` (Block::dec_ref, add flag constant, declare append_user_data_with_meta)
- Modify: `src-common/fast_iobuf.cc` (implement append_user_data_with_meta)
- Create: `test/unit/test_iobuf_user_data.cc` (纯逻辑测试)

**Interfaces:**
- Consumes: `UserDataDeleter` (已存在), `UserDataExtension` (已存在), `Block::get_user_data_extension()` (已存在)
- Produces: `int IOBuf::append_user_data_with_meta(void* data, size_t size, UserDataDeleter deleter, uint64_t meta)`, `IOBUF_BLOCK_FLAGS_USER_DATA = 1`

**当前已有基础设施：**
- `UserDataDeleter` = `std::function<void(void*)>` 已定义 (`fast_iobuf_inl.h:15`)
- `UserDataExtension { UserDataDeleter deleter; }` 已定义 (`fast_iobuf_inl.h:17-20`)
- `Block::get_user_data_extension()` 已实现 — 返回 `(UserDataExtension*)(this + 1)` (`fast_iobuf_inl.h:160-164`)
- `Block` 有 `flags` 字段 (`fast_iobuf_inl.h:144`)

- [ ] **Step 1: 添加 flag 常量** (`inc/fast_iobuf_inl.h`)

```cpp
// 在 UserDataDeleter 定义之后添加:
constexpr uint16_t IOBUF_BLOCK_FLAGS_USER_DATA = 1;
```

- [ ] **Step 2: 修改 `dec_ref`** — 支持 user data block 析构 (`inc/fast_iobuf_inl.h:182-191`)

当前代码:
```cpp
void dec_ref()
{
    check_abi();
    if (nshared.fetch_sub(1, std::memory_order_release) == 1)
    {
        std::atomic_thread_fence(std::memory_order_acquire);
        this->~Block();
        fast::blockmem_deallocate(this);
    }
}
```

改为:
```cpp
void dec_ref()
{
    check_abi();
    if (nshared.fetch_sub(1, std::memory_order_release) == 1)
    {
        std::atomic_thread_fence(std::memory_order_acquire);
        if (flags & IOBUF_BLOCK_FLAGS_USER_DATA) {
            auto ext = get_user_data_extension();
            ext->deleter(data);
            ext->~UserDataExtension();
            this->~Block();
            free(this);                       // user data block: free()
        } else {
            this->~Block();
            fast::blockmem_deallocate(this);  // BlockPool block: deallocate
        }
    }
}
```

- [ ] **Step 3: 在 `inc/fast_iobuf_inl.h` 中添加 `append_user_data_with_meta` 声明**

在 IOBuf 类中 (public 区域) 添加:
```cpp
int append_user_data_with_meta(void* data, size_t size,
                                UserDataDeleter deleter,
                                uint64_t meta);
```

- [ ] **Step 4: 在 `src-common/fast_iobuf.cc` 中实现 `append_user_data_with_meta`**

参考 brpc `iobuf.cpp:1121`:

```cpp
int IOBuf::append_user_data_with_meta(void* data,
                                       size_t size,
                                       UserDataDeleter deleter,
                                       uint64_t meta) {
    if (size > 0xFFFFFFFFULL - 100) {
        LOG(FATAL) << "data_size=" << size << " is too large";
        return -1;
    }
    if (!deleter) {
        deleter = ::free;
    }
    if (!size) {
        deleter(data);
        return 0;
    }
    char* mem = (char*)malloc(sizeof(IOBuf::Block) + sizeof(UserDataExtension));
    if (mem == NULL) {
        return -1;
    }
    IOBuf::Block* b = new (mem) IOBuf::Block((char*)data, size);
    b->flags |= IOBUF_BLOCK_FLAGS_USER_DATA;
    b->u.data_meta = meta;
    b->cap = size;
    b->size = size;
    UserDataExtension* ext = b->get_user_data_extension();
    new (ext) UserDataExtension{std::move(deleter)};
    const IOBuf::BlockRef r = { 0, (uint32_t)size, b };
    _move_back_ref(r);
    return 0;
}
```

- [ ] **Step 5: 写单元测试** (`test/unit/test_iobuf_user_data.cc`)

```cpp
#include <gtest/gtest.h>
#include "fast_iobuf.h"

namespace fast {

TEST(IOBufUserDataTest, AppendAndDestroyCallsDeleter) {
    bool deleted = false;
    char* data = new char[1024];
    memset(data, 'A', 1024);

    {
        IOBuf buf;
        buf.append_user_data_with_meta(data, 1024,
            [&deleted](void* p) { deleted = true; delete[] (char*)p; },
            42);
        EXPECT_EQ(buf.length(), 1024u);
        EXPECT_EQ(buf.fetch1()[0], 'A');
        // meta 可通过 get_first_data_meta 查询
    }
    // buf 析构 → dec_ref → deleter 调用
    EXPECT_TRUE(deleted);
}

TEST(IOBufUserDataTest, CutnPreservesDeleter) {
    bool deleted = false;
    char* data = new char[1024];
    memset(data, 'X', 1024);

    IOBuf buf;
    buf.append_user_data_with_meta(data, 1024,
        [&deleted](void* p) { deleted = true; delete[] (char*)p; },
        0);

    IOBuf cut;
    buf.cutn(&cut, 100);
    buf.clear();  // buf 放弃全部引用

    EXPECT_FALSE(deleted);  // cut 还持有引用
    EXPECT_EQ(cut.length(), 100u);
    cut.clear();  // 最后一个引用释放
    EXPECT_TRUE(deleted);
}

}  // namespace fast
```

- [ ] **Step 6: 更新 CMakeLists.txt** — 添加测试源文件

```cmake
# unit_tests 目标自动 GLOB test/unit/*.cc，新文件自动包含
```

- [ ] **Step 7: 编译 + 运行测试**

```bash
cd build && make && ./unit_tests --gtest_filter='IOBufUserDataTest*'
```

Expected: 2 tests PASS

- [ ] **Step 8: Commit**

---

### Task 3: LargeBlock TLS 缓存

**Files:**
- Create: `inc/fast_large_block.h`
- Create: `src-common/fast_large_block.cc`
- Create: `test/unit/test_large_block.cc` (纯逻辑测试 — 不涉及 ibv_reg_mr，测试 best-fit/LRU/TLS 逻辑)

**Interfaces:**
- Consumes: `g_pd` (global PD, from `fast_rdma_endpoint.cc` — 需要声明为 extern)
- Produces: `ibv_mr* LargeBlockAlloc(size_t size)`, `void ReturnLargeBlock(ibv_mr* mr)`

> **注意**: `LargeBlockAlloc`/`ReturnLargeBlock` 内部调用 `ibv_reg_mr`/`ibv_dereg_mr`，属于 RDMA verbs。单元测试仅测试纯逻辑的 best-fit/LRU 算法（通过 mock/提取纯逻辑辅助函数实现），集成测试不做。

**实现**：直接复用 `src-resource/fast_shared.cc` 中已验证的 `LargeBlockAlloc`/`ReturnLargeBlock` 实现，仅将 `cm_id_->pd` 替换为 `extern ibv_pd* g_pd`。

`inc/fast_large_block.h`:
```cpp
#pragma once
#include <infiniband/verbs.h>
#include <cstddef>

namespace fast {
ibv_mr* LargeBlockAlloc(size_t size);
void ReturnLargeBlock(ibv_mr* mr);
}  // namespace fast
```

`src-common/fast_large_block.cc` — 从 `src-resource/fast_shared.cc:17-254` 迁移，关键修改:
- `#include "fast_large_block.h"`
- 用 `extern ibv_pd* g_pd;` 替代 `cm_id_->pd`
- `ibv_reg_mr(g_pd, buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE)`

- [ ] **Step 1: 创建 `inc/fast_large_block.h`**

- [ ] **Step 2: 创建 `src-common/fast_large_block.cc`** — 从 `fast_shared.cc` 迁移 LargeBlockNode + LargeBlockAlloc + ReturnLargeBlock + RecycleTLSLargeBlocks

- [ ] **Step 3: 在 `src-endpoint/fast_rdma_endpoint.cc` 中将 `g_pd` 改为 `extern`**（去除 `static`，或提供 `GetGlobalPd()` 访问器）

- [ ] **Step 4: 编译验证**

```bash
cd build && make
```

Expected: 编译通过，链接成功。

- [ ] **Step 5: Commit**

---

### Task 4: HelloMessage 扩展 + Data QP 握手

**Files:**
- Modify: `inc/fast_rdma_endpoint.h` (HelloMessage + data_qp_, Serialize/Deserialize)
- Modify: `src-endpoint/fast_rdma_endpoint.cc` (HelloMessage Ser/Des, AllocateResources 重构, BringUpQp Data QP, CreateCq/CreateQp helpers)
- Modify: `test/unit/test_rdma_endpoint.cc` (HelloMessage 序列化 44B 测试)

**Interfaces:**
- Consumes: `g_pd`, `g_ctx`, `g_rdma_max_sge`
- Produces:
  - `HelloMessage::kMsgLen = 44`, `uint32_t data_qp_num`
  - `static ibv_cq* CreateCq(int size, ibv_comp_channel* ch)`
  - `static ibv_qp* CreateQp(ibv_cq* scq, ibv_cq* rcq, int swr, int rwr, int sge_s, int sge_r)`
  - `FastRdmaEndpoint::data_qp_`, `data_send_cq_`, `data_recv_cq_`

- [ ] **Step 1: 修改 HelloMessage** (`inc/fast_rdma_endpoint.h`)

```cpp
struct HelloMessage {
    static const size_t   kMsgLen = 44;  // was 40
    // ... existing fields ...
    uint32_t qp_num = 0;
    uint32_t data_qp_num = 0;  // +4B, appended after qp_num
    // ...
};
```

- [ ] **Step 2: 更新 Serialize/Deserialize**

Serialize 末尾:
```cpp
uint32_t* dqpn = reinterpret_cast<uint32_t*>(qpn + 1);
*dqpn = htonl(data_qp_num);
```

Deserialize 末尾:
```cpp
const uint32_t* dqpn = reinterpret_cast<const uint32_t*>(qpn + 1);
data_qp_num = ntohl(*dqpn);
```

- [ ] **Step 3: 添加 CreateCq / CreateQp 静态 helper** (`src-endpoint/fast_rdma_endpoint.cc`)

```cpp
namespace {
static ibv_cq* CreateCq(int size, ibv_comp_channel* ch) {
    ibv_cq* cq = ibv_create_cq(g_ctx, size, nullptr, ch, 0);
    CHECK(cq != nullptr);
    return cq;
}

static ibv_qp* CreateQp(ibv_cq* send_cq, ibv_cq* recv_cq,
                         int max_send_wr, int max_recv_wr,
                         int max_send_sge, int max_recv_sge) {
    ibv_qp_init_attr attr = {};
    attr.send_cq = send_cq;
    attr.recv_cq = recv_cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr  = max_send_wr;
    attr.cap.max_recv_wr  = max_recv_wr;
    attr.cap.max_send_sge = max_send_sge;
    attr.cap.max_recv_sge = max_recv_sge;
    ibv_qp* qp = ibv_create_qp(g_pd, &attr);
    CHECK(qp != nullptr);
    return qp;
}
}  // namespace
```

- [ ] **Step 4: 重构 AllocateResources**

```cpp
int FastRdmaEndpoint::AllocateResources() {
    comp_channel_ = ibv_create_comp_channel(g_ctx);
    CHECK(comp_channel_ != nullptr);

    send_cq_      = CreateCq(sq_size_, comp_channel_);
    recv_cq_      = CreateCq(rq_size_, comp_channel_);
    data_send_cq_ = CreateCq(kDataQpDepth, comp_channel_);
    data_recv_cq_ = CreateCq(kDataQpDepth, comp_channel_);

    qp_      = CreateQp(send_cq_, recv_cq_, sq_size_, rq_size_, g_rdma_max_sge, 1);
    data_qp_ = CreateQp(data_send_cq_, data_recv_cq_, kDataQpDepth, kDataQpDepth, g_rdma_max_sge, 1);

    sbuf_.resize(sq_size_ - RESERVED_WR_NUM);
    rbuf_.resize(rq_size_);
    rbuf_data_.resize(rq_size_, nullptr);

    ibv_req_notify_cq(send_cq_, 0);
    ibv_req_notify_cq(recv_cq_, 1);
    ibv_req_notify_cq(data_send_cq_, 0);
    ibv_req_notify_cq(data_recv_cq_, 1);

    EventDispatcher::GetInstance().RegisterEvent(
        comp_channel_->fd, OnCompChannelEvent, nullptr, this, EPOLLIN | EPOLLET);

    return 0;
}
```

- [ ] **Step 5: 添加 BringUpDataQp 逻辑** (`BringUpQp` 扩展或新增独立函数)

```cpp
// Data QP: RESET→INIT→RTR→RTS
// 与 Control QP 差异:
//   qp_access_flags = IBV_ACCESS_REMOTE_WRITE
//   rnr_retry = 0
//   PostRecv: 不 post（按需）
//   sq_psn / rq_psn = 0（独立 PSN）
```

- [ ] **Step 6: 更新握手 — ProcessHandshakeAtClient/Server**

- 交换 HelloMessage 包含 data_qp_num
- BringUpQp(Control QP) 后调用 BringUpQp(Data QP)

- [ ] **Step 7: 更新单元测试** (`test/unit/test_rdma_endpoint.cc`)

```cpp
TEST(HelloMessageTest, SerializeDeserialize44B) {
    HelloMessage msg;
    msg.data_qp_num = 42;
    uint8_t buf[HelloMessage::kMsgLen];
    msg.Serialize(buf);
    HelloMessage msg2;
    msg2.Deserialize(buf);
    EXPECT_EQ(msg2.data_qp_num, 42u);
    EXPECT_EQ(msg2.qp_num, msg.qp_num);
}
```

- [ ] **Step 8: 编译 + 测试**

```bash
cd build && make && ./unit_tests
```

- [ ] **Step 9: 更新 DeallocateResources**

```cpp
void FastRdmaEndpoint::DeallocateResources() {
    // ... existing cleanup ...
    if (data_qp_)      { ibv_destroy_qp(data_qp_);      data_qp_ = nullptr; }
    if (data_send_cq_) { ibv_destroy_cq(data_send_cq_);  data_send_cq_ = nullptr; }
    if (data_recv_cq_) { ibv_destroy_cq(data_recv_cq_);  data_recv_cq_ = nullptr; }
}
```

- [ ] **Step 10: Commit**

---

### Task 5: FastRdmaEndpoint Large 传输资源管理

**Files:**
- Modify: `inc/fast_rdma_endpoint.h` (pending maps, flow control, helpers 声明)
- Modify: `src-endpoint/fast_rdma_endpoint.cc` (PostLargeWriteRecv, CutSegFromIOBuf, Store/Get/ReleaseLargeFrame, 流控方法)

**Interfaces:**
- Produces:
  - `int PostLargeWriteRecv(ibv_mr* mr)`
  - `ssize_t CutSegFromIOBuf(IOBuf*, uint32_t remote_rkey, uint64_t remote_addr, uint32_t imm_rkey, uint32_t rpc_id)`
  - `void StoreLargeFrame(uint32_t, IOBuf&&)`, `IOBuf* GetLargeFrame(uint32_t)`, `void ReleaseLargeFrame(uint32_t)`
  - `bool CanStartLargeTransfer()`, `void StartLargeTransfer()`, `void OnLargeTransferComplete()`
  - `std::function<void()> _large_done_cb`
  - `std::unordered_map<uint32_t, IOBuf> pending_large_frames_`
  - `std::unordered_map<uint32_t, ibv_mr*> pending_large_map_` (key = rkey)

> **注意**: PostLargeWriteRecv 和 CutSegFromIOBuf 内部使用 ibv_post_recv / ibv_post_send，不写单元测试。

- [ ] **Step 1: 添加字段声明** (`inc/fast_rdma_endpoint.h`)

```cpp
// Data QP 资源 (Task 4 已添加)
static constexpr int kDataQpDepth = 8;
static constexpr int kMaxLargeTransfers = 8;

// Large 传输流控
std::atomic<int> active_large_transfers_{0};
bool CanStartLargeTransfer() const {
    return active_large_transfers_.load(std::memory_order_relaxed) < kMaxLargeTransfers;
}
void StartLargeTransfer() {
    active_large_transfers_.fetch_add(1, std::memory_order_relaxed);
}
void OnLargeTransferComplete() {
    active_large_transfers_.fetch_sub(1, std::memory_order_relaxed);
    if (_large_done_cb) _large_done_cb();
}
std::function<void()> _large_done_cb;

// Large 帧存储
std::mutex large_frame_mutex_;
std::unordered_map<uint32_t, IOBuf> pending_large_frames_;
void StoreLargeFrame(uint32_t rpc_id, IOBuf&& frame);
IOBuf* GetLargeFrame(uint32_t rpc_id);
void ReleaseLargeFrame(uint32_t rpc_id);

// LargeBlock 跟踪
std::mutex pending_large_mutex_;
std::unordered_map<uint32_t, ibv_mr*> pending_large_map_;  // key = rkey

// Helpers
int PostLargeWriteRecv(ibv_mr* mr);
ssize_t CutSegFromIOBuf(IOBuf* buf, uint32_t remote_rkey, uint64_t remote_addr,
                         uint32_t imm_rkey, uint32_t rpc_id);
```

- [ ] **Step 2: 实现 PostLargeWriteRecv**

```cpp
int FastRdmaEndpoint::PostLargeWriteRecv(ibv_mr* mr) {
    ibv_sge sge = {};
    sge.addr   = reinterpret_cast<uint64_t>(mr->addr);
    sge.length = 4;
    sge.lkey   = mr->lkey;
    ibv_recv_wr wr = {};
    wr.sg_list = &sge;
    wr.num_sge = 1;
    ibv_recv_wr* bad = nullptr;
    return ibv_post_recv(data_qp_, &wr, &bad);
}
```

- [ ] **Step 3: 实现 CutSegFromIOBuf**

```cpp
ssize_t FastRdmaEndpoint::CutSegFromIOBuf(IOBuf* buf,
                                           uint32_t remote_rkey,
                                           uint64_t remote_addr,
                                           uint32_t imm_rkey,
                                           uint32_t rpc_id) {
    RdmaIOBuf* rio = static_cast<RdmaIOBuf*>(buf);
    ibv_sge sglist[MAX_SGE];
    size_t sge_idx = 0;
    size_t total = 0;
    // 遍历 IOBuf block refs → 提取 sge（同 cut_into_sglist_and_iobuf，不 cut）
    for (size_t i = 0; i < rio->ref_num() && sge_idx < MAX_SGE; ++i) {
        const BlockRef& ref = rio->ref_at(i);
        sglist[sge_idx].addr   = reinterpret_cast<uint64_t>(ref.block->data + ref.offset);
        sglist[sge_idx].length = ref.length;
        sglist[sge_idx].lkey   = GetRegionId(ref.block->data + ref.offset);
        total += ref.length;
        ++sge_idx;
    }

    ibv_send_wr wr = {};
    wr.opcode              = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.imm_data            = htonl(imm_rkey);
    wr.wr_id               = rpc_id;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;
    wr.sg_list             = sglist;
    wr.num_sge             = static_cast<int>(sge_idx);

    ibv_send_wr* bad = nullptr;
    int ret = ibv_post_send(data_qp_, &wr, &bad);
    return (ret == 0) ? static_cast<ssize_t>(total) : -1;
}
```

- [ ] **Step 4: 实现 StoreLargeFrame / GetLargeFrame / ReleaseLargeFrame**

```cpp
void FastRdmaEndpoint::StoreLargeFrame(uint32_t rpc_id, IOBuf&& frame) {
    std::lock_guard<std::mutex> lock(large_frame_mutex_);
    pending_large_frames_[rpc_id] = std::move(frame);
}

IOBuf* FastRdmaEndpoint::GetLargeFrame(uint32_t rpc_id) {
    std::lock_guard<std::mutex> lock(large_frame_mutex_);
    auto it = pending_large_frames_.find(rpc_id);
    return (it != pending_large_frames_.end()) ? &it->second : nullptr;
}

void FastRdmaEndpoint::ReleaseLargeFrame(uint32_t rpc_id) {
    std::lock_guard<std::mutex> lock(large_frame_mutex_);
    pending_large_frames_.erase(rpc_id);
}
```

- [ ] **Step 5: 编译**

```bash
cd build && make
```

Expected: 编译通过（新增方法暂未被调用）。

- [ ] **Step 6: Commit**

---

### Task 6: PollCq 4 CQ 轮询 + GetAndAckEvents 扩展

**Files:**
- Modify: `src-endpoint/fast_rdma_endpoint.cc` (`PollCq`, `GetAndAckEvents`)

**Interfaces:**
- Consumes: Task 4 (data_qp_, data_send_cq_, data_recv_cq_), Task 5 (pending_large_map_, pending_large_frames_, OnLargeTransferComplete)
- Produces: 4 CQ 轮询逻辑，Data recv/send inline 处理

> **注意**: PollCq 内部涉及 ibv_poll_cq / ibv_post_recv / ibv_req_notify_cq，不写单元测试。

- [ ] **Step 1: 扩展 GetAndAckEvents** — 识别 4 CQ

```cpp
int FastRdmaEndpoint::GetAndAckEvents() {
    static const int MAX_CQ_EVENTS = 128;
    while (true) {
        ibv_cq* cq = nullptr;
        void*   ctx = nullptr;
        if (ibv_get_cq_event(comp_channel_, &cq, &ctx) != 0) {
            if (errno == EAGAIN) break;
            LOG_ERR("Fail to get cq event");
            return -1;
        }
        if (cq == send_cq_)            ++send_cq_events;
        else if (cq == recv_cq_)       ++recv_cq_events;
        else if (cq == data_send_cq_)  ++data_send_cq_events;
        else if (cq == data_recv_cq_)  ++data_recv_cq_events;
        else LOG_ERR("Unknown CQ event");
    }
    // ack 4 CQ
    if (send_cq_events >= MAX_CQ_EVENTS)      { ibv_ack_cq_events(send_cq_, send_cq_events); send_cq_events = 0; }
    if (recv_cq_events >= MAX_CQ_EVENTS)      { ibv_ack_cq_events(recv_cq_, recv_cq_events); recv_cq_events = 0; }
    if (data_send_cq_events >= MAX_CQ_EVENTS) { ibv_ack_cq_events(data_send_cq_, data_send_cq_events); data_send_cq_events = 0; }
    if (data_recv_cq_events >= MAX_CQ_EVENTS) { ibv_ack_cq_events(data_recv_cq_, data_recv_cq_events); data_recv_cq_events = 0; }
    return 0;
}
```

- [ ] **Step 2: 重构 PollCq** — 替换为 Phase 1/2/3 结构

```cpp
void FastRdmaEndpoint::PollCq(FastRdmaEndpoint* ep) {
    if (ep->_stop.load(std::memory_order_relaxed)) return;

    if (ep->GetAndAckEvents() < 0) return;

    bool notified = false;
    ibv_wc wc[32];
    int progress = PROGRESS_INIT;

    while (true) {
        if (ep->_stop.load(std::memory_order_relaxed)) return;

        bool any_recv = false;

        // ---- Phase 1: 所有 recv ----
        {
            int cnt = ibv_poll_cq(ep->recv_cq_, 32, wc);
            if (cnt < 0) return;
            for (int i = 0; i < cnt; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) continue;
                ep->HandleCompletion(wc[i]);  // read_buf_ 积累 + 流控
            }
            if (cnt > 0) any_recv = true;
        }

        {
            int cnt = ibv_poll_cq(ep->data_recv_cq_, 32, wc);
            if (cnt < 0) return;
            for (int i = 0; i < cnt; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) continue;
                // inline Data QP recv 处理
                uint32_t rkey = ntohl(wc[i].imm_data);
                {
                    std::lock_guard<std::mutex> lock(ep->pending_large_mutex_);
                    auto it = ep->pending_large_map_.find(rkey);
                    if (it != ep->pending_large_map_.end()) {
                        ibv_mr* mr = it->second;
                        ep->pending_large_map_.erase(it);
                        // 零拷贝包装 LargeBlock 为 IOBuf → 融入 read_buf_
                        IOBuf frame;
                        frame.append_user_data_with_meta(
                            mr->addr, wc[i].byte_len,
                            [](void* p) { ReturnLargeBlock((ibv_mr*)p); }, 0);
                        ep->read_buf_.append(std::move(frame));
                    }
                }
                // 不 re-post recv
            }
            if (cnt > 0) any_recv = true;
        }

        // 统一分发（Control + Data 帧）
        if (any_recv) {
            ep->_msg_dispatcher.ProcessNewMessage(ep->read_buf_);
            continue;  // 有数据，继续 recv 阶段
        }

        // ---- Phase 2: 所有 send ----
        {
            int cnt = ibv_poll_cq(ep->send_cq_, 32, wc);
            if (cnt < 0) return;
            for (int i = 0; i < cnt; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) continue;
                ep->HandleCompletion(wc[i]);
            }
        }

        {
            int cnt = ibv_poll_cq(ep->data_send_cq_, 32, wc);
            if (cnt < 0) return;
            for (int i = 0; i < cnt; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) continue;
                // inline Data QP send 处理
                uint32_t rpc_id = static_cast<uint32_t>(wc[i].wr_id);
                ep->ReleaseLargeFrame(rpc_id);
                ep->OnLargeTransferComplete();
            }
            if (cnt > 0 && !any_recv) continue;  // send 有产出，回到 recv 阶段
        }

        // ---- Phase 3: re-arm + re-poll ----
        if (!notified) {
            ibv_req_notify_cq(ep->send_cq_, 0);
            ibv_req_notify_cq(ep->recv_cq_, 1);
            ibv_req_notify_cq(ep->data_send_cq_, 0);
            ibv_req_notify_cq(ep->data_recv_cq_, 1);
            notified = true;
            continue;
        }

        if (!ep->MoreReadEvents(&progress)) break;

        if (ep->GetAndAckEvents() < 0) return;
        notified = false;
    }
}
```

- [ ] **Step 3: 编译**

```bash
cd build && make
```

Expected: 编译通过。

- [ ] **Step 4: Commit**

---

### Task 7: FastChannel Large 路径

**Files:**
- Modify: `inc/fast_channel.h` (新增 large flow control 字段)
- Modify: `src-endpoint/fast_channel.cc` (CallMethod Large 分支, OnProcessResponse 三分支)

**Interfaces:**
- Consumes: Task 5 (StoreLargeFrame/GetLargeFrame/ReleaseLargeFrame/CutSegFromIOBuf), Task 2 (append_user_data_with_meta)
- Produces: `WaitForLargeWritable()`, OnProcessResponse 中 MSG_NOTIFY / MSG_AUTHORITY / MSG_NORMAL_RESPONSE 分支

- [ ] **Step 1: 修改 `inc/fast_channel.h`**

```cpp
class FastChannel : public google::protobuf::RpcChannel {
    // ... existing ...
private:
    void WaitForLargeWritable();

    std::mutex              large_mutex_;
    std::condition_variable large_cv_;
    bool                    closed_{false};

    struct PendingRequest {
        // ... existing fields ...
        bool is_large = false;
    };
};
```

实现 `WaitForLargeWritable`:
```cpp
void FastChannel::WaitForLargeWritable() {
    std::unique_lock<std::mutex> lock(large_mutex_);
    large_cv_.wait(lock, [this] {
        return endpoint_->CanStartLargeTransfer() || closed_;
    });
    if (!closed_) endpoint_->StartLargeTransfer();
}
```

- [ ] **Step 2: 构造/析构修改**

构造中注册回调:
```cpp
FastChannel::FastChannel(std::string dest_ip, int dest_port) {
    // ... existing ...
    endpoint_->_large_done_cb = [this] { large_cv_.notify_one(); };
}
```

析构中:
```cpp
FastChannel::~FastChannel() {
    closed_ = true;
    large_cv_.notify_all();
    // ... existing cleanup ...
}
```

- [ ] **Step 3: CallMethod Large 路径**

在 CallMethod 中，total_len >= msg_threshold 的分支:

```cpp
// Large 路径
pending.is_large = true;
WaitForLargeWritable();  // Data QP 深度准入
// 注意: 还需 WaitForWritable（Control QP 流控）
// ...

// 保存完整帧到 endpoint
endpoint_->StoreLargeFrame(rpc_id, std::move(frame));

// 构建 MSG_NOTIFY 帧
IOBuf notify_frame;
uint32_t be;
be = htonl(kNotifyFrameBytes);      notify_frame.append(&be, 4);  // total_len
be = htonl(MSG_NOTIFY);              notify_frame.append(&be, 4);  // msg_type
be = htonl(rpc_id);                  notify_frame.append(&be, 4);  // rpc_id
be = htonl(total_len);               notify_frame.append(&be, 4);  // data_total_len
endpoint_->StartWrite(std::move(notify_frame));
// cv.wait() — 唯一阻塞点
```

- [ ] **Step 4: OnProcessResponse 三分支**

```cpp
int FastChannel::OnProcessResponse(IOBuf& frame, void* arg) {
    auto* self = static_cast<FastChannel*>(arg);
    frame.pop_front(4);  // skip total_len (already consumed by CutInputMessage)

    uint32_t msg_type = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    uint32_t rpc_id = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    // --- MSG_NOTIFY: Server 要发 Large 响应 ---
    if (msg_type == MSG_NOTIFY) {
        uint32_t data_total_len = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        ibv_mr* mr = LargeBlockAlloc(data_total_len);
        self->endpoint_->PostLargeWriteRecv(mr);

        IOBuf auth_frame;
        uint32_t be;
        be = htonl(kAuthFrameBytes);     auth_frame.append(&be, 4);
        be = htonl(MSG_AUTHORITY);        auth_frame.append(&be, 4);
        be = htonl(rpc_id);               auth_frame.append(&be, 4);
        be = htonl(mr->rkey);             auth_frame.append(&be, 4);
        uint64_t addr = reinterpret_cast<uint64_t>(mr->addr);
        be = htonl(addr >> 32);           auth_frame.append(&be, 4);
        be = htonl(addr & 0xFFFFFFFF);    auth_frame.append(&be, 4);
        self->endpoint_->StartWrite(std::move(auth_frame));

        {
            std::lock_guard<std::mutex> lock(self->endpoint_->pending_large_mutex_);
            self->endpoint_->pending_large_map_[mr->rkey] = mr;
        }
        return 0;
    }

    // --- MSG_AUTHORITY: Server 回复了 LargeBlock 地址 ---
    if (msg_type == MSG_AUTHORITY) {
        uint32_t rkey = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        frame.pop_front(4);
        uint32_t remote_addr_hi = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        frame.pop_front(4);
        uint32_t remote_addr_lo = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        uint64_t remote_addr = (static_cast<uint64_t>(remote_addr_hi) << 32) | remote_addr_lo;

        IOBuf* large_frame = self->endpoint_->GetLargeFrame(rpc_id);
        self->endpoint_->CutSegFromIOBuf(large_frame, rkey, remote_addr, rkey, rpc_id);
        return 0;
    }

    // --- MSG_NORMAL_RESPONSE: 现有逻辑 ---
    // ... (existing code unchanged) ...
    // 不操作 active_large_transfers_——counter 在 PollCq 中管理
}
```

- [ ] **Step 5: 编译**

```bash
cd build && make
```

Expected: 编译通过。

- [ ] **Step 6: Commit**

---

### Task 8: FastServer Large 路径

**Files:**
- Modify: `inc/fast_server.h` (新增 large flow control 字段)
- Modify: `src-endpoint/fast_server.cc` (OnProcessRequest 三分支, ReturnRPCResponse Large 分支)

**Interfaces:**
- Consumes: Task 5, Task 2
- Produces: Server-side MSG_NOTIFY/MSG_AUTHORITY handling, Large response initiation

- [ ] **Step 1: 修改 `inc/fast_server.h`**

```cpp
class FastServer {
    // ... existing ...
private:
    void WaitForLargeWritable();

    std::mutex              large_mutex_;
    std::condition_variable large_cv_;
    bool                    closed_{false};
};
```

- [ ] **Step 2: 构造/析构**

```cpp
FastServer::FastServer(std::string local_ip, int local_port)
    : local_ip_(std::move(local_ip)), local_port_(local_port) {}

FastServer::~FastServer() {
    closed_ = true;
    large_cv_.notify_all();
    // ... existing cleanup ...
}
```

`OnServerAccept` 中注册回调:
```cpp
ep->_large_done_cb = [server] { server->large_cv_.notify_one(); };
```

- [ ] **Step 3: OnProcessRequest 三分支**

新增 MSG_NOTIFY 和 MSG_AUTHORITY 分支:

```cpp
int FastServer::OnProcessRequest(IOBuf& frame, void* arg) {
    auto* ep = static_cast<FastRdmaEndpoint*>(arg);
    auto* server = ep->owner();
    if (server == nullptr) return -1;

    uint32_t total_len = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    uint32_t msg_type = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    uint32_t rpc_id = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    // === MSG_NOTIFY: Client 有 Large 请求 ===
    if (msg_type == MSG_NOTIFY) {
        uint32_t data_total_len = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        ibv_mr* mr = LargeBlockAlloc(data_total_len);
        ep->PostLargeWriteRecv(mr);

        IOBuf auth_frame;
        uint32_t be;
        be = htonl(kAuthFrameBytes);     auth_frame.append(&be, 4);
        be = htonl(MSG_AUTHORITY);        auth_frame.append(&be, 4);
        be = htonl(rpc_id);               auth_frame.append(&be, 4);
        be = htonl(mr->rkey);             auth_frame.append(&be, 4);
        uint64_t addr = reinterpret_cast<uint64_t>(mr->addr);
        be = htonl(static_cast<uint32_t>(addr >> 32));    auth_frame.append(&be, 4);
        be = htonl(static_cast<uint32_t>(addr & 0xFFFFFFFF)); auth_frame.append(&be, 4);
        ep->StartWrite(std::move(auth_frame));

        {
            std::lock_guard<std::mutex> lock(ep->pending_large_mutex_);
            ep->pending_large_map_[mr->rkey] = mr;
        }
        return 0;
    }

    // === MSG_AUTHORITY: Client 回复了 LargeBlock 地址（Server 要发 Large 响应）===
    if (msg_type == MSG_AUTHORITY) {
        uint32_t rkey = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        frame.pop_front(4);
        uint32_t hi = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        frame.pop_front(4);
        uint32_t lo = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        uint64_t remote_addr = (static_cast<uint64_t>(hi) << 32) | lo;

        IOBuf* large_frame = ep->GetLargeFrame(rpc_id);
        ep->CutSegFromIOBuf(large_frame, rkey, remote_addr, rkey, rpc_id);
        return 0;
    }

    // === MSG_NORMAL: 现有逻辑（不变）===
    // ... (existing OnProcessRequest code for MSG_NORMAL) ...
}
```

- [ ] **Step 4: ReturnRPCResponse Large 响应分支**

```cpp
void FastServer::ReturnRPCResponse(CallBackArgs args) {
    // ... existing code to build frame ...
    // [total_len=16(BE)][rpc_id(BE)][error_code(BE)][attachment_size(BE)][payload][attachment]

    uint32_t total_len = 16 + payload_len + attachment_len;

    if (total_len >= msg_threshold) {
        // Large 响应
        args.endpoint->StoreLargeFrame(args.rpc_id, std::move(frame));
        WaitForLargeWritable();

        IOBuf notify_frame;
        uint32_t be;
        be = htonl(kNotifyFrameBytes);  notify_frame.append(&be, 4);
        be = htonl(MSG_NOTIFY);          notify_frame.append(&be, 4);
        be = htonl(args.rpc_id);         notify_frame.append(&be, 4);
        be = htonl(total_len);           notify_frame.append(&be, 4);
        args.endpoint->StartWrite(std::move(notify_frame));
        // MSG_AUTHORITY 到达时 OnProcessRequest 触发 CutSegFromIOBuf
        return;
    }

    // Medium 响应：现有逻辑
    args.endpoint->StartWrite(std::move(frame));
}
```

- [ ] **Step 5: 编译**

```bash
cd build && make
```

Expected: 编译通过。

- [ ] **Step 6: Commit**

---

### Task 9: Proto 文件更新 + 整体验证

**Files:**
- Modify: `proto/fast_impl.proto` (ResponseHead 添加 msg_type)
- Modify: `CMakeLists.txt` (确保新文件已包含)
- Modify: `test/server.cc`, `test/client.cc` (适配新接口，如需要)

- [ ] **Step 1: Proto 更新**

```proto
message ResponseHead {
  fixed32 total_len = 1;
  fixed32 msg_type = 2;    // 新增
  fixed32 rpc_id = 3;
  uint32 attachment_size = 4;
}
```

- [ ] **Step 2: 确认 CMakeLists.txt 包含新源文件**

```cmake
set(COMMON_SOURCES
    src-common/fast_block_pool.cc
    src-common/fast_define.cc
    src-common/fast_iobuf.cc
    src-common/fast_verbs.cc
    src-common/fast_large_block.cc   # 新增
)
```

- [ ] **Step 3: 最终编译**

```bash
cd build && cmake .. && make
```

Expected: 编译通过，无警告。

- [ ] **Step 4: 运行单元测试**

```bash
./unit_tests
```

Expected: 全部通过。

- [ ] **Step 5: Commit**

---

## Dependencies

```
Task 1 (msg_type) ─────────────────────────────────────────────────────┐
Task 2 (IOBuf append_user_data) ───────────────────────────────────────┤
                                                                        │
Task 3 (LargeBlock cache) ─────────────────────────────────────────────┤
Task 4 (HelloMessage + Data QP 握手) ─────────────────────────────────┤
                                                                        │
Task 5 (pending maps + helpers) ← depends on Tasks 1,2,3,4 ──────────┤
                                                                        │
Task 6 (PollCq 4 CQ) ← depends on Tasks 4,5 ──────────────────────────┤
Task 7 (FastChannel) ← depends on Tasks 1,2,5 ────────────────────────┤
Task 8 (FastServer) ← depends on Tasks 1,2,5 ─────────────────────────┤
                                                                        │
Task 9 (Proto + final verify) ← depends on all ───────────────────────┘
```

Tasks 7 和 8 可并行执行（不互相依赖）。Tasks 1-4 之间无依赖可部分并行。
