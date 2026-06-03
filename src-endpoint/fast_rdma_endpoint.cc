#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include "fast_block_pool.h"
#include "fast_log.h"
#include "fast_rdma_endpoint.h"

namespace fast {

// ============================================================
// Global test flag
// ============================================================

bool g_skip_rdma_init = false;

static const int RESERVED_WR_NUM = 3;

// ============================================================
// HelloMessage serialization (network byte order)
// ============================================================

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
    msg_len    = ntohs(*pos++);
    hello_ver  = ntohs(*pos++);
    impl_ver   = ntohs(*pos++);
    const uint32_t* bs = reinterpret_cast<const uint32_t*>(pos);
    block_size = ntohl(*bs);
    pos += 2;
    sq_size = ntohs(*pos++);
    rq_size = ntohs(*pos++);
    lid     = ntohs(*pos++);
    memcpy(gid, pos, 16);
    pos += 8;
    const uint32_t* qpn = reinterpret_cast<const uint32_t*>(pos);
    qp_num = ntohl(*qpn);
}

bool HelloNegotiationValid(const HelloMessage& msg) {
    static const uint16_t kMinQpSize    = 16;
    static const uint32_t kMinBlockSize = 1024;
    return msg.hello_ver == HelloMessage::kHelloVer &&
           msg.impl_ver != 0 &&
           msg.block_size >= kMinBlockSize &&
           msg.sq_size >= kMinQpSize &&
           msg.rq_size >= kMinQpSize;
}

// ============================================================
// FastRdmaEndpoint
// ============================================================

FastRdmaEndpoint::FastRdmaEndpoint() = default;

FastRdmaEndpoint::~FastRdmaEndpoint() {
    DeallocateResources();
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

void FastRdmaEndpoint::SetNegotiatedParams(
        uint16_t sq_size, uint16_t rq_size,
        uint16_t remote_sq_size, uint16_t remote_rq_size,
        uint32_t block_size) {
    sq_size_               = sq_size;
    rq_size_               = rq_size;
    remote_recv_block_size_ = block_size;
    local_window_capacity_  = std::min<int>(sq_size_, remote_rq_size) - RESERVED_WR_NUM;
    remote_window_capacity_ = std::min<int>(rq_size_, remote_sq_size) - RESERVED_WR_NUM;
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

// ---------------------------------------------------------------------------
// Flow control
// ---------------------------------------------------------------------------

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
        return SendImm(static_cast<uint32_t>(total));
    }
    return 0;
}

int FastRdmaEndpoint::SendImm(uint32_t imm) {
    if (imm == 0 || g_skip_rdma_init) return 0;
    // TODO: ibv_post_send with IBV_WR_SEND_WITH_IMM, wr_id=0
    sq_imm_window_size_ -= 1;
    return 0;
}

// ---------------------------------------------------------------------------
// TCP I/O helpers
// ---------------------------------------------------------------------------

int FastRdmaEndpoint::ReadFromFd(int fd, void* data, size_t len) {
    size_t received = 0;
    char*  buf      = static_cast<char*>(data);
    while (received < len) {
        ssize_t nr = read(fd, buf + received, len - received);
        if (nr <= 0) return -1;
        received += static_cast<size_t>(nr);
    }
    return 0;
}

int FastRdmaEndpoint::WriteToFd(int fd, const void* data, size_t len) {
    size_t written = 0;
    const char* buf = static_cast<const char*>(data);
    while (written < len) {
        ssize_t nw = write(fd, buf + written, len - written);
        if (nw <= 0) return -1;
        written += static_cast<size_t>(nw);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

int FastRdmaEndpoint::ProcessHandshakeAtClient(int tcp_fd) {
    if (g_skip_rdma_init) return 0;

    // 1. Allocate CQ + QP
    if (AllocateResources(sq_size_, rq_size_) < 0) return -1;

    // 2. Send HelloMessage
    HelloMessage local;
    local.block_size = remote_recv_block_size_ == 0 ? 8192 : remote_recv_block_size_;
    local.sq_size   = sq_size_;
    local.rq_size   = rq_size_;
    local.qp_num    = qp_->qp_num;

    uint8_t data[HelloMessage::kMsgLen];
    local.Serialize(data);
    if (WriteToFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    // 3. Receive remote HelloMessage
    if (ReadFromFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;
    HelloMessage remote;
    remote.Deserialize(data);

    if (memcmp(remote.magic, "RDMA", 4) != 0) return -1;
    if (!HelloNegotiationValid(remote)) return -1;

    // 4. Set negotiated params
    remote_recv_block_size_ = remote.block_size;
    local_window_capacity_  = std::min<int>(sq_size_, remote.rq_size) - RESERVED_WR_NUM;
    remote_window_capacity_ = std::min<int>(rq_size_, remote.sq_size) - RESERVED_WR_NUM;
    sq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    remote_rq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    sq_imm_window_size_ = RESERVED_WR_NUM;

    // 5. Bring up QP (RESET->INIT->RTR->RTS)
    ibv_gid gid;
    memcpy(gid.raw, remote.gid, 16);
    if (BringUpQp(remote.lid, gid, remote.qp_num) < 0) return -1;

    // 6. Post initial recv WRs
    sbuf_.resize(sq_size_ - RESERVED_WR_NUM);
    rbuf_.resize(rq_size_);
    rbuf_data_.resize(rq_size_, nullptr);
    if (PostRecv(rq_size_, false) < 0) return -1;

    // 7. Send ACK (RDMA_OK)
    uint32_t ack = htonl(1);
    if (WriteToFd(tcp_fd, &ack, 4) < 0) return -1;

    return 0;
}

int FastRdmaEndpoint::ProcessHandshakeAtServer(int tcp_fd) {
    if (g_skip_rdma_init) return 0;

    // 1. Read client HelloMessage
    uint8_t data[HelloMessage::kMsgLen];
    if (ReadFromFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    HelloMessage remote;
    remote.Deserialize(data);
    if (memcmp(remote.magic, "RDMA", 4) != 0) return -1;
    if (!HelloNegotiationValid(remote)) return -1;

    // 2. Allocate resources
    if (AllocateResources(sq_size_, rq_size_) < 0) return -1;

    // 3. Send HelloMessage
    HelloMessage local;
    local.block_size = remote_recv_block_size_ == 0 ? 8192 : remote_recv_block_size_;
    local.sq_size   = sq_size_;
    local.rq_size   = rq_size_;
    local.qp_num    = qp_ ? qp_->qp_num : 0;

    local.Serialize(data);
    if (WriteToFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    // 4. Set negotiated params
    remote_recv_block_size_ = remote.block_size;
    local_window_capacity_  = std::min<int>(sq_size_, remote.rq_size) - RESERVED_WR_NUM;
    remote_window_capacity_ = std::min<int>(rq_size_, remote.sq_size) - RESERVED_WR_NUM;
    sq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    remote_rq_window_size_.store(local_window_capacity_, std::memory_order_relaxed);
    sq_imm_window_size_ = RESERVED_WR_NUM;

    // 5. Bring up QP
    ibv_gid gid;
    memcpy(gid.raw, remote.gid, 16);
    if (BringUpQp(remote.lid, gid, remote.qp_num) < 0) return -1;

    sbuf_.resize(sq_size_ - RESERVED_WR_NUM);
    rbuf_.resize(rq_size_);
    rbuf_data_.resize(rq_size_, nullptr);
    if (PostRecv(rq_size_, false) < 0) return -1;

    // 6. Wait for ACK
    uint32_t ack;
    if (ReadFromFd(tcp_fd, &ack, 4) < 0) return -1;

    return 0;
}

// ---------------------------------------------------------------------------
// RDMA resource management
// ---------------------------------------------------------------------------

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
    // TODO: destroy qp, cqs, comp_channel via ibv_* calls
    sbuf_.clear();
    rbuf_.clear();
    rbuf_data_.clear();
}

// ---------------------------------------------------------------------------
// Recv
// ---------------------------------------------------------------------------

int FastRdmaEndpoint::DoPostRecv(void* block, size_t block_size) {
    if (g_skip_rdma_init) return 0;
    // TODO: ibv_post_recv
    return 0;
}

int FastRdmaEndpoint::PostRecv(uint32_t num, bool /*zerocopy*/) {
    if (g_skip_rdma_init) {
        new_rq_wrs_.fetch_add(num, std::memory_order_relaxed);
        return 0;
    }
    // TODO: prepare buffers and post recv WRs to RQ
    new_rq_wrs_.fetch_add(num, std::memory_order_relaxed);
    return 0;
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

ssize_t FastRdmaEndpoint::CutFromIOBufList(IOBuf** from, size_t ndata) {
    if (g_skip_rdma_init) {
        for (size_t i = 0; i < ndata && from[i]; ++i) {
            SimulateSendOne();
        }
        return static_cast<ssize_t>(ndata);
    }
    // TODO: real implementation
    // - iterate from[0..ndata-1]
    // - call RdmaIOBuf::cut_into_sglist_and_iobuf for each IOBuf
    // - build ibv_send_wr with solicited/signaled flags
    // - ibv_post_send
    // - update sq_current_, decrement windows
    return 0;
}

// ---------------------------------------------------------------------------
// CQ
// ---------------------------------------------------------------------------

int FastRdmaEndpoint::comp_channel_fd() const {
    return comp_channel_ ? comp_channel_->fd : -1;
}

void FastRdmaEndpoint::PollCq() {
    if (g_skip_rdma_init) return;
    // TODO: ibv_poll_cq loop -> HandleCompletion for each WC
}

ssize_t FastRdmaEndpoint::HandleCompletion(ibv_wc& wc) {
    switch (wc.opcode) {
    case IBV_WC_SEND:
        if (wc.wr_id == 0) {
            // Pure ACK / IMM send completed
            sq_imm_window_size_ += 1;
            SendAck(0);
            return 0;
        }
        // Signaled batch send: release sbuf slots, restore SQ window
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
        return static_cast<ssize_t>(wc.byte_len);

    default:
        return -1;
    }
}

// ============================================================
// RdmaIOBuf
// ============================================================

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

}  // namespace fast
