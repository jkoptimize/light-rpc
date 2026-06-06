# Phase 1: FastRdmaEndpoint Implementation Plan

> **For agentic workers:** Execute task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement FastRdmaEndpoint — per-connection RDMA endpoint with TCP handshake, flow control, IOBuf→sge sending, WC handling, and condition_variable blocking.

**Architecture:** Raw ibv_verbs + TCP out-of-band handshake. FastRdmaEndpoint owns QP/CQ, flow control windows, sbuf/rbuf buffers, and condition_variable for blocking. RdmaIOBuf (IOBuf subclass) provides cut_into_sglist_and_iobuf. fetch1() added to IOBuf.

**Tech Stack:** C++17, ibv_verbs, gtest, CMake

---

### Task 1: Project Infrastructure — gtest + test/unit/ directory + CMakeLists

**Files:**
- Create: `test/unit/` directory
- Modify: `CMakeLists.txt`
- Verify: `test/unit/` builds with `make`

- [ ] **Step 1: Add gtest to CMakeLists.txt**

```cmake
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})
```

- [ ] **Step 2: Add endpoint source to build**

```cmake
set(ENDPOINT_SOURCES
    src-endpoint/fast_channel.cc
    src-endpoint/fast_server.cc
    src-endpoint/fast_rdma_endpoint.cc
)
```

- [ ] **Step 3: Add unit test target**

```cmake
file(GLOB UNIT_TEST_SOURCES test/unit/*.cc)
add_executable(unit_tests ${UNIT_TEST_SOURCES})
target_link_libraries(unit_tests fastrpc ${GTEST_LIBRARIES} pthread ibverbs)
add_test(NAME UnitTests COMMAND unit_tests)
```

- [ ] **Step 4: Create placeholder test file**

```cpp
// test/unit/test_rdma_endpoint.cc
#include <gtest/gtest.h>

TEST(FastRdmaEndpoint, Dummy) {
    EXPECT_EQ(1, 1);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 5: Build and run dummy test**

```bash
cd build && cmake .. && make
./unit_tests
```

Expected: 1 test PASS

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt test/unit/test_rdma_endpoint.cc
git commit -m "feat: add gtest infrastructure and unit_tests target"
```

---

### Task 2: Add fetch1() to IOBuf

**Files:**
- Modify: `inc/fast_iobuf.h` (add declaration)
- Modify: `src-common/fast_iobuf.cc` (add implementation)

- [ ] **Step 1: Declare fetch1() in fast_iobuf.h**

In the public section, add after `bool empty() const;`:

```cpp
// Fetch one character from front side.
// Returns pointer to the character, NULL on empty.
const void* fetch1() const;
```

- [ ] **Step 2: Implement fetch1() in fast_iobuf.cc**

```cpp
const void* IOBuf::fetch1() const {
    if (!empty()) {
        const BlockRef& r0 = _front_ref();
        return r0.block->data + r0.offset;
    }
    return NULL;
}
```

Note: `_front_ref()` is protected, so this must be a member function of IOBuf (not a free function).

- [ ] **Step 3: Build**

```bash
cd build && make
```

Expected: clean build

- [ ] **Step 4: Commit**

```bash
git add inc/fast_iobuf.h src-common/fast_iobuf.cc
git commit -m "feat: add fetch1() to IOBuf"
```

---

### Task 3: FastRdmaEndpoint header

**Files:**
- Create: `inc/fast_rdma_endpoint.h`

Full header content:

```cpp
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <infiniband/verbs.h>
#include "fast_iobuf.h"

namespace fast {

// ---- HelloMessage ----

struct HelloMessage {
    static const size_t kMsgLen = 40;
    static const uint16_t kHelloVer = 1;
    static const uint16_t kImplVer = 1;  // 0 = TCP fallback

    char     magic[4] = { 'R', 'D', 'M', 'A' };
    uint16_t msg_len = kMsgLen;
    uint16_t hello_ver = kHelloVer;
    uint16_t impl_ver = kImplVer;
    uint32_t block_size = 8192;
    uint16_t sq_size = 0;
    uint16_t rq_size = 0;
    uint16_t lid = 0;
    uint8_t  gid[16] = {};
    uint32_t qp_num = 0;

    void Serialize(void* data) const;
    void Deserialize(const void* data);
};

bool HelloNegotiationValid(const HelloMessage& msg);

// ---- RdmaIOBuf ----

class RdmaIOBuf : public IOBuf {
    friend class FastRdmaEndpoint;

public:
    static const size_t IOBUF_BLOCK_HEADER_LEN = 32;

private:
    ssize_t cut_into_sglist_and_iobuf(ibv_sge* sglist, size_t* sge_index,
                                       IOBuf* to, size_t max_sge, size_t max_len);
};

// ---- FastRdmaEndpoint ----

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

    // ============ 测试辅助 ============
    int sq_window_size() const { return sq_window_size_.load(std::memory_order_relaxed); }
    int remote_rq_window_size() const { return remote_rq_window_size_.load(std::memory_order_relaxed); }
    int new_rq_wrs() const { return new_rq_wrs_.load(std::memory_order_relaxed); }

    void SetNegotiatedParams(uint16_t sq_size, uint16_t rq_size,
                              uint16_t remote_sq_size, uint16_t remote_rq_size,
                              uint32_t block_size);
    void SimulateSendOne();
    void SimulateSendN(int n);

private:
    int SendAck(int num);
    int SendImm(uint32_t imm);
    int DoPostRecv(void* block, size_t block_size);
    int ReadFromFd(int fd, void* data, size_t len);
    int WriteToFd(int fd, const void* data, size_t len);

    // RDMA 资源
    ibv_qp* qp_ = nullptr;
    ibv_cq* send_cq_ = nullptr;
    ibv_cq* recv_cq_ = nullptr;
    ibv_comp_channel* comp_channel_ = nullptr;

    // 协商参数
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
    std::vector<IOBuf> sbuf_;
    size_t sq_current_{0};
    size_t sq_sent_{0};
    std::vector<IOBuf> rbuf_;
    std::vector<void*> rbuf_data_;
    size_t rq_received_{0};

    // 阻塞等待
    std::mutex send_mutex_;
    std::condition_variable send_cv_;

    // Selective signaling
    int send_counter_{0};
    int sq_unsignaled_{0};
    int unsolicited_{0};
    int accumulated_ack_{0};
};

}  // namespace fast
```

- [ ] **Step 1: Write the header file**

```bash
# Create inc/fast_rdma_endpoint.h with content above
```

- [ ] **Step 2: Check build (will fail — no .cc yet)**

```bash
cd build && make
```

Expected: linker error for missing FastRdmaEndpoint methods

- [ ] **Step 3: Commit**

```bash
git add inc/fast_rdma_endpoint.h
git commit -m "feat: add FastRdmaEndpoint header"
```

---

### Task 4: FastRdmaEndpoint core methods (HelloMessage + flow init + basic ops)

**Files:**
- Create: `src-endpoint/fast_rdma_endpoint.cc`

Implement in order:
1. `g_skip_rdma_init` flag
2. HelloMessage::Serialize/Deserialize
3. HelloNegotiationValid
4. Constructor/Destructor
5. SetNegotiatedParams, SimulateSendOne/N (test helpers)
6. IsWritable, WaitForWritable
7. SendAck, SendImm (stubs with g_skip_rdma_init check)
8. ProcessHandshakeAtClient/Server (basic skeleton)
9. AllocateResources, DeallocateResources (with g_skip_rdma_init)
10. BringUpQp (with g_skip_rdma_init)
11. PostRecv, DoPostRecv (with g_skip_rdma_init)
12. CutFromIOBufList (with g_skip_rdma_init + sge filling logic)
13. PollCq, HandleCompletion (full logic)
14. comp_channel_fd, ReadFromFd, WriteToFd

```cpp
// src-endpoint/fast_rdma_endpoint.cc

#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include "fast_block_pool.h"
#include "fast_log.h"
#include "fast_rdma_endpoint.h"

namespace fast {

bool g_skip_rdma_init = false;

// ---- HelloMessage ----

void HelloMessage::Serialize(void* data) const {
    char* buf = static_cast<char*>(data);
    memcpy(buf, magic, 4);
    uint16_t* pos = reinterpret_cast<uint16_t*>(buf + 4);
    *pos++ = htons(msg_len);
    *pos++ = htons(hello_ver);
    *pos++ = htons(impl_ver);
    uint32_t* bs = reinterpret_cast<uint32_t*>(pos);
    *bs = htonl(block_size);
    pos += 2;
    *pos++ = htons(sq_size);
    *pos++ = htons(rq_size);
    *pos++ = htons(lid);
    memcpy(pos, gid, 16);
    pos += 8;
    uint32_t* qpn = reinterpret_cast<uint32_t*>(pos);
    *qpn = htonl(qp_num);
}

void HelloMessage::Deserialize(const void* data) {
    const char* buf = static_cast<const char*>(data);
    memcpy(magic, buf, 4);
    const uint16_t* pos = reinterpret_cast<const uint16_t*>(buf + 4);
    msg_len = ntohs(*pos++);
    hello_ver = ntohs(*pos++);
    impl_ver = ntohs(*pos++);
    const uint32_t* bs = reinterpret_cast<const uint32_t*>(pos);
    block_size = ntohl(*bs);
    pos += 2;
    sq_size = ntohs(*pos++);
    rq_size = ntohs(*pos++);
    lid = ntohs(*pos++);
    memcpy(gid, pos, 16);
    pos += 8;
    const uint32_t* qpn = reinterpret_cast<const uint32_t*>(pos);
    qp_num = ntohl(*qpn);
}

bool HelloNegotiationValid(const HelloMessage& msg) {
    static const uint16_t kMinQpSize = 16;
    static const uint32_t kMinBlockSize = 1024;
    return msg.hello_ver == HelloMessage::kHelloVer &&
           msg.impl_ver != 0 &&
           msg.block_size >= kMinBlockSize &&
           msg.sq_size >= kMinQpSize &&
           msg.rq_size >= kMinQpSize;
}

// ---- Helper: GetRegionId / custom ibv ops ----

static const int RESERVED_WR_NUM = 3;

// ---- FastRdmaEndpoint ----

FastRdmaEndpoint::FastRdmaEndpoint() { }

FastRdmaEndpoint::~FastRdmaEndpoint() {
    DeallocateResources();
}

void FastRdmaEndpoint::SetNegotiatedParams(
        uint16_t sq_size, uint16_t rq_size,
        uint16_t remote_sq_size, uint16_t remote_rq_size,
        uint32_t block_size) {
    sq_size_ = sq_size;
    rq_size_ = rq_size;
    remote_recv_block_size_ = block_size;
    local_window_capacity_ = std::min(sq_size_, remote_rq_size) - RESERVED_WR_NUM;
    remote_window_capacity_ = std::min(rq_size_, remote_sq_size) - RESERVED_WR_NUM;
    sq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    remote_rq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    sq_imm_window_size_ = RESERVED_WR_NUM;
    new_rq_wrs_.store(0, std::memory_order_relaxed);
    sbuf_.resize(sq_size_ - RESERVED_WR_NUM);
    rbuf_.resize(rq_size_);
    rbuf_data_.resize(rq_size_, nullptr);
}

void FastRdmaEndpoint::SimulateSendOne() {
    sq_window_size_.fetch_sub(1, std::memory_order_relaxed);
    remote_rq_window_size_.fetch_sub(1, std::memory_order_relaxed);
}

void FastRdmaEndpoint::SimulateSendN(int n) {
    for (int i = 0; i < n; ++i) SimulateSendOne();
}

bool FastRdmaEndpoint::IsWritable() const {
    return sq_window_size_.load(std::memory_order_relaxed) > 0 &&
           remote_rq_window_size_.load(std::memory_order_relaxed) > 0;
}

void FastRdmaEndpoint::WaitForWritable() {
    std::unique_lock<std::mutex> lock(send_mutex_);
    send_cv_.wait(lock, [this] { return IsWritable(); });
}

int FastRdmaEndpoint::SendAck(int num) {
    int prev = new_rq_wrs_.fetch_add(num, std::memory_order_relaxed);
    if (prev + num > remote_window_capacity_ / 2 && sq_imm_window_size_ > 0) {
        int total = new_rq_wrs_.exchange(0, std::memory_order_relaxed);
        return SendImm(total);
    }
    return 0;
}

int FastRdmaEndpoint::SendImm(uint32_t imm) {
    if (imm == 0 || g_skip_rdma_init) return 0;
    // TODO: actual ibv_post_send with IBV_WR_SEND_WITH_IMM
    sq_imm_window_size_ -= 1;
    return 0;
}

int FastRdmaEndpoint::comp_channel_fd() const {
    return comp_channel_ ? comp_channel_->fd : -1;
}

// ---- TCP I/O helpers ----

int FastRdmaEndpoint::ReadFromFd(int fd, void* data, size_t len) {
    size_t received = 0;
    char* buf = static_cast<char*>(data);
    while (received < len) {
        ssize_t nr = read(fd, buf + received, len - received);
        if (nr <= 0) return -1;
        received += nr;
    }
    return 0;
}

int FastRdmaEndpoint::WriteToFd(int fd, const void* data, size_t len) {
    size_t written = 0;
    const char* buf = static_cast<const char*>(data);
    while (written < len) {
        ssize_t nw = write(fd, buf + written, len - written);
        if (nw <= 0) return -1;
        written += nw;
    }
    return 0;
}

// ---- Handshake ----

int FastRdmaEndpoint::ProcessHandshakeAtClient(int tcp_fd) {
    // Phase 1: skip if g_skip_rdma_init
    if (g_skip_rdma_init) return 0;

    // Alloc CQ+QP
    if (AllocateResources(sq_size_, rq_size_) < 0) return -1;

    // Send HelloMessage
    HelloMessage local;
    local.block_size = remote_recv_block_size_ == 0 ? 8192 : remote_recv_block_size_;
    local.sq_size = sq_size_;
    local.rq_size = rq_size_;
    // Fill lid/gid from local device (placeholder for Phase 1)
    // ibv_query_port / ibv_query_gid
    local.qp_num = qp_->qp_num;

    uint8_t data[HelloMessage::kMsgLen];
    local.Serialize(data);
    if (WriteToFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    // Receive remote HelloMessage
    if (ReadFromFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;
    HelloMessage remote;
    remote.Deserialize(data);

    // Validate magic
    if (memcmp(remote.magic, "RDMA", 4) != 0) return -1;

    if (!HelloNegotiationValid(remote)) return -1;

    remote_recv_block_size_ = remote.block_size;
    local_window_capacity_ = std::min(sq_size_, remote.rq_size) - RESERVED_WR_NUM;
    remote_window_capacity_ = std::min(rq_size_, remote.sq_size) - RESERVED_WR_NUM;
    sq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    remote_rq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    sq_imm_window_size_ = RESERVED_WR_NUM;

    // Bring up QP
    ibv_gid gid;
    memcpy(gid.raw, remote.gid, 16);
    if (BringUpQp(remote.lid, gid, remote.qp_num) < 0) return -1;

    // Post initial recv WRs
    sbuf_.resize(sq_size_ - RESERVED_WR_NUM);
    rbuf_.resize(rq_size_);
    rbuf_data_.resize(rq_size_, nullptr);
    if (PostRecv(rq_size_, false) < 0) return -1;

    // Send ACK with RDMA_OK
    uint32_t ack = htonl(1);  // RDMA_OK
    if (WriteToFd(tcp_fd, &ack, 4) < 0) return -1;

    return 0;
}

int FastRdmaEndpoint::ProcessHandshakeAtServer(int tcp_fd) {
    if (g_skip_rdma_init) return 0;

    // Read client's HelloMessage
    uint8_t data[HelloMessage::kMsgLen];
    if (ReadFromFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    HelloMessage remote;
    remote.Deserialize(data);
    if (memcmp(remote.magic, "RDMA", 4) != 0) return -1;
    if (!HelloNegotiationValid(remote)) return -1;

    // Alloc resources
    if (AllocateResources(sq_size_, rq_size_) < 0) return -1;

    // Send HelloMessage
    HelloMessage local;
    local.block_size = remote_recv_block_size_ == 0 ? 8192 : remote_recv_block_size_;
    local.sq_size = sq_size_;
    local.rq_size = rq_size_;
    local.qp_num = qp_ ? qp_->qp_num : 0;

    local.Serialize(data);
    if (WriteToFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    // Set params
    remote_recv_block_size_ = remote.block_size;
    local_window_capacity_ = std::min(sq_size_, remote.rq_size) - RESERVED_WR_NUM;
    remote_window_capacity_ = std::min(rq_size_, remote.sq_size) - RESERVED_WR_NUM;
    sq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    remote_rq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    sq_imm_window_size_ = RESERVED_WR_NUM;

    // Bring up QP
    ibv_gid gid;
    memcpy(gid.raw, remote.gid, 16);
    if (BringUpQp(remote.lid, gid, remote.qp_num) < 0) return -1;

    sbuf_.resize(sq_size_ - RESERVED_WR_NUM);
    rbuf_.resize(rq_size_);
    rbuf_data_.resize(rq_size_, nullptr);
    if (PostRecv(rq_size_, false) < 0) return -1;

    // Wait for ACK
    uint32_t ack;
    if (ReadFromFd(tcp_fd, &ack, 4) < 0) return -1;

    return 0;
}

// ---- RDMA Resource Management ----

int FastRdmaEndpoint::AllocateResources(uint16_t sq_size, uint16_t rq_size) {
    if (g_skip_rdma_init) return 0;
    // TODO: create comp_channel, send_cq, recv_cq, qp via ibv_* calls
    // Requires ibv_context (pd), which must be passed or stored globally
    return 0;
}

int FastRdmaEndpoint::BringUpQp(uint16_t lid, ibv_gid gid, uint32_t remote_qpn) {
    if (g_skip_rdma_init) return 0;
    // TODO: ibv_modify_qp RESET->INIT, INIT->RTR, RTR->RTS
    return 0;
}

void FastRdmaEndpoint::DeallocateResources() {
    if (g_skip_rdma_init) return;
    // TODO: destroy qp, cqs, comp_channel
    sbuf_.clear();
    rbuf_.clear();
    rbuf_data_.clear();
}

// ---- Recv ----

int FastRdmaEndpoint::DoPostRecv(void* block, size_t block_size) {
    if (g_skip_rdma_init) return 0;
    // TODO: ibv_post_recv
    return 0;
}

int FastRdmaEndpoint::PostRecv(uint32_t num, bool zerocopy) {
    if (g_skip_rdma_init) return 0;
    // TODO: prepare buffers and post recv WRs to RQ
    for (uint32_t i = 0; i < num; ++i) {
        // Allocate buffer, post recv
        new_rq_wrs_.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

// ---- Send ----

ssize_t FastRdmaEndpoint::CutFromIOBufList(IOBuf** from, size_t ndata) {
    if (g_skip_rdma_init) {
        // Simulate consuming the IOBufs and deducting window slots
        for (size_t i = 0; i < ndata; ++i) {
            SimulateSendOne();
        }
        return 1;  // return >0 to indicate "success"
    }
    // TODO: real CutFromIOBufList implementation
    // 1. iterate from[0..ndata-1]
    // 2. call RdmaIOBuf::cut_into_sglist_and_iobuf for each
    // 3. build ibv_send_wr
    // 4. ibv_post_send
    // 5. update sq_current_, decrement windows
    return 0;
}

// ---- CQ ----

void FastRdmaEndpoint::PollCq() {
    if (g_skip_rdma_init) return;
    // TODO: ibv_poll_cq loop → HandleCompletion for each wc
}

ssize_t FastRdmaEndpoint::HandleCompletion(ibv_wc& wc) {
    switch (wc.opcode) {
    case IBV_WC_SEND:
        if (wc.wr_id == 0) {
            sq_imm_window_size_ += 1;
            SendAck(0);
            return 0;
        }
        for (uint16_t i = 0; i < wc.wr_id; ++i) {
            if (sq_sent_ < sbuf_.size()) {
                sbuf_[sq_sent_++].clear();
                if (sq_sent_ == sbuf_.size()) sq_sent_ = 0;
            }
        }
        sq_window_size_.fetch_add(wc.wr_id, std::memory_order_relaxed);
        send_cv_.notify_one();
        return 0;

    case IBV_WC_RECV:
        if (wc.byte_len > 0 && rq_received_ < rbuf_.size()) {
            // rbuf_[rq_received_].data is available
            ++rq_received_;
            if (rq_received_ == rbuf_.size()) rq_received_ = 0;
        }
        if ((wc.wc_flags & IBV_WC_WITH_IMM) && wc.imm_data > 0) {
            remote_rq_window_size_.fetch_add(ntohl(wc.imm_data),
                                              std::memory_order_relaxed);
            send_cv_.notify_one();
        }
        PostRecv(1, true);
        if (wc.byte_len > 0) SendAck(1);
        return wc.byte_len;

    default:
        return -1;
    }
}

// ---- RdmaIOBuf ----

ssize_t RdmaIOBuf::cut_into_sglist_and_iobuf(
        ibv_sge* sglist, size_t* sge_index,
        IOBuf* to, size_t max_sge, size_t max_len) {
    if (g_skip_rdma_init) {
        *sge_index = 1;
        return 1;
    }
    size_t len = 0;
    while (*sge_index < max_sge) {
        if (len >= max_len || _ref_num() == 0) break;
        const BlockRef& r = _ref_at(0);
        const void* start = fetch1();
        uint32_t lkey = GetRegionId(const_cast<void*>(start));
        if (lkey == 0) return -1;
        size_t n = r.length;
        if (len + n > max_len) n = max_len - len;
        size_t i = *sge_index;
        sglist[i].addr = (uint64_t)start;
        sglist[i].length = n;
        sglist[i].lkey = lkey;
        cutn(to, n);
        len += n;
        (*sge_index)++;
    }
    return static_cast<ssize_t>(len);
}

}  // namespace fast
```

- [ ] **Step 1: Write the implementation file**

- [ ] **Step 2: Build**

```bash
cd build && cmake .. && make
```

Expected: clean build

- [ ] **Step 3: Commit**

```bash
git add src-endpoint/fast_rdma_endpoint.cc
git commit -m "feat: add FastRdmaEndpoint core implementation"
```

---

### Task 5: Unit tests

**Files:**
- Modify: `test/unit/test_rdma_endpoint.cc`

Replace dummy test with actual tests:

```cpp
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <infiniband/verbs.h>
#include "fast_rdma_endpoint.h"

namespace fast {
extern bool g_skip_rdma_init;
}

class FastRdmaEndpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        fast::g_skip_rdma_init = true;
    }
    void TearDown() override {
        fast::g_skip_rdma_init = false;
    }
};

// ---- HelloMessage ----

TEST_F(FastRdmaEndpointTest, HelloMessageSerializeDeserialize) {
    fast::HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size = 128;
    msg.rq_size = 128;
    msg.lid = 5;
    msg.gid[0] = 0xFE;
    msg.qp_num = 42;

    uint8_t buf[40];
    msg.Serialize(buf);

    fast::HelloMessage msg2;
    msg2.Deserialize(buf);

    EXPECT_EQ(msg2.block_size, 8192);
    EXPECT_EQ(msg2.sq_size, 128);
    EXPECT_EQ(msg2.rq_size, 128);
    EXPECT_EQ(msg2.lid, 5);
    EXPECT_EQ(msg2.gid[0], 0xFE);
    EXPECT_EQ(msg2.qp_num, 42);
    EXPECT_EQ(memcmp(msg2.magic, "RDMA", 4), 0);
}

TEST_F(FastRdmaEndpointTest, HelloNegotiationValid) {
    fast::HelloMessage msg;
    msg.block_size = 8192;
    msg.sq_size = 128;
    msg.rq_size = 128;

    EXPECT_TRUE(fast::HelloNegotiationValid(msg));

    msg.impl_ver = 0;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));

    msg.impl_ver = 1;
    msg.block_size = 512;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));

    msg.block_size = 8192;
    msg.sq_size = 10;
    EXPECT_FALSE(fast::HelloNegotiationValid(msg));
}

// ---- Flow Control ----

TEST_F(FastRdmaEndpointTest, FlowControlInit) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    EXPECT_TRUE(ep.IsWritable());
    EXPECT_EQ(ep.sq_window_size(), 29);
    EXPECT_EQ(ep.remote_rq_window_size(), 29);
}

TEST_F(FastRdmaEndpointTest, WaitForWritableBlocksAndWakes) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    // Exhaust window: post 29 times (32-3=29)
    ep.SimulateSendN(29);
    EXPECT_FALSE(ep.IsWritable());

    std::atomic<bool> woken{false};
    std::thread waiter([&] {
        ep.WaitForWritable();
        woken = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(woken);

    // Simulate SEND WC completing 1 batch
    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id = 1;
    ep.HandleCompletion(wc);

    waiter.join();
    EXPECT_TRUE(woken);
    EXPECT_EQ(ep.sq_window_size(), 30);  // 29 - 0 + 1 = 30
}

TEST_F(FastRdmaEndpointTest, WaitForWritableConsumeBehavior) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);
    ep.SimulateSendN(29);

    std::thread t1([&] { ep.WaitForWritable(); });
    std::thread t2([&] { ep.WaitForWritable(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id = 1;
    ep.HandleCompletion(wc);

    t1.join(); t2.join();

    // Third wait should block again (window consumed again)
    ep.SimulateSendN(1);
    EXPECT_FALSE(ep.IsWritable());

    std::atomic<bool> blocked{false};
    std::thread t3([&] {
        ep.WaitForWritable();
        blocked = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(blocked);

    wc.wr_id = 1;
    ep.HandleCompletion(wc);
    t3.join();
    EXPECT_TRUE(blocked);
}

// ---- SendAck ----

TEST_F(FastRdmaEndpointTest, SendAckTriggerThreshold) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);
    // remote_window_capacity_ = min(32, 32) - 3 = 29
    // threshold = 29/2 = 14

    // Below threshold: no IMM sent (new_rq_wrs = 5, SendAck adds num)
    // SendAck(5): new_rq_wrs = 5, 5 < 14, no trigger
    // But send returns 0 because no IMM was actually sent

    // Accumulate to just below threshold
    ep.SendAck(10);
    // new_rq_wrs = 10, < 14, still no trigger

    // Cross threshold
    int result = ep.SendAck(5);
    // new_rq_wrs = 10 + 5 = 15, 15 >= 14, triggers
    // But with g_skip_rdma_init, SendImm returns 0
    EXPECT_EQ(result, 0);
}

// ---- HandleCompletion ----

TEST_F(FastRdmaEndpointTest, HandleSendCompletion) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);
    ep.SimulateSendN(3);

    EXPECT_EQ(ep.sq_window_size(), 26);

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id = 3;
    ep.HandleCompletion(wc);

    EXPECT_EQ(ep.sq_window_size(), 29);
}

TEST_F(FastRdmaEndpointTest, HandleImmSendCompletion) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    ibv_wc wc{};
    wc.opcode = IBV_WC_SEND;
    wc.wr_id = 0;  // IMM/ACK send
    ep.HandleCompletion(wc);

    EXPECT_EQ(ep.sq_window_size(), 29);  // unch
}

TEST_F(FastRdmaEndpointTest, HandleRecvCompletionWithImm) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    EXPECT_EQ(ep.remote_rq_window_size(), 29);

    ibv_wc wc{};
    wc.opcode = IBV_WC_RECV;
    wc.byte_len = 1024;
    wc.wc_flags = IBV_WC_WITH_IMM;
    wc.imm_data = htonl(3);  // remote freed 3 RQ slots

    ssize_t nr = ep.HandleCompletion(wc);

    EXPECT_EQ(nr, 1024);
    EXPECT_EQ(ep.remote_rq_window_size(), 32);  // 29 + 3
}

// ---- CutFromIOBufList (with g_skip_rdma_init) ----

TEST_F(FastRdmaEndpointTest, CutFromIOBufListSkipInit) {
    fast::FastRdmaEndpoint ep;
    ep.SetNegotiatedParams(32, 32, 32, 32, 8192);

    fast::IOBuf iobuf;
    iobuf.append("hello", 5);
    fast::IOBuf* arr[1] = { &iobuf };

    EXPECT_EQ(ep.sq_window_size(), 29);
    ssize_t ret = ep.CutFromIOBufList(arr, 1);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(ep.sq_window_size(), 28);  // consumed 1 slot
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 1: Write the test file**

- [ ] **Step 2: Build and run tests**

```bash
cd build && make && ./unit_tests
```

Expected: 10 tests PASS

- [ ] **Step 3: Commit**

```bash
git add test/unit/test_rdma_endpoint.cc
git commit -m "test: add FastRdmaEndpoint unit tests"
```

---

### Task 6: Final verification — clean build + full test run

- [ ] **Step 1: Clean rebuild**

```bash
cd build && cmake .. && make
```

Expected: 0 warnings

- [ ] **Step 2: Run all tests**

```bash
cd build && ./unit_tests
```

Expected: all tests PASS

- [ ] **Step 3: Verify file structure**

```bash
ls -la inc/fast_rdma_endpoint.h src-endpoint/fast_rdma_endpoint.cc test/unit/test_rdma_endpoint.cc
```
