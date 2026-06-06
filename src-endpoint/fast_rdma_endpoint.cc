#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include "fast_block_pool.h"
#include "fast_log.h"
#include "fast_rdma_endpoint.h"

namespace fast {

static const int RESERVED_WR_NUM = 3;

// ---- Global RDMA resources (set once by GlobalInitialize) ----
static ibv_context* g_ctx    = nullptr;
static ibv_pd*      g_pd     = nullptr;
static ibv_gid      g_gid    = {};
static uint16_t     g_lid    = 0;
static int          g_rdma_max_sge = 32;
static uint32_t     g_rdma_recv_block_size = 0;
static uint32_t     g_rdma_zerocopy_min_size = 512;

void FastRdmaEndpoint::GlobalInitialize() {
    ibv_device** devs = ibv_get_device_list(nullptr);
    CHECK(devs != nullptr && devs[0] != nullptr);
    g_ctx = ibv_open_device(devs[0]);
    CHECK(g_ctx != nullptr);
    ibv_free_device_list(devs);

    g_pd = ibv_alloc_pd(g_ctx);
    CHECK(g_pd != nullptr);

    ibv_device_attr attr;
    CHECK(ibv_query_device(g_ctx, &attr) == 0);
    g_rdma_max_sge = attr.max_sge;

    ibv_port_attr port_attr;
    CHECK(ibv_query_port(g_ctx, 1, &port_attr) == 0);
    g_lid = port_attr.lid;

    // RoCE: take GID at index 0
    CHECK(ibv_query_gid(g_ctx, 1, 0, &g_gid) == 0);

    g_rdma_recv_block_size = GetBlockSize(0) - RdmaIOBuf::IOBUF_BLOCK_HEADER_LEN;
}

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
// Test helpers — exposed only for unit tests, see AGENTS.md / CLAUDE.md
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
    if (imm == 0) return 0;

    ibv_send_wr wr = {};
    wr.opcode     = IBV_WR_SEND_WITH_IMM;
    wr.imm_data   = htonl(imm);
    wr.send_flags = IBV_SEND_SOLICITED | IBV_SEND_SIGNALED;
    wr.wr_id      = 0;

    ibv_send_wr* bad = nullptr;
    int ret = ibv_post_send(qp_, &wr, &bad);
    if (ret != 0) return -1;

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

int FastRdmaEndpoint::ProcessHandshakeAtClient(FastRdmaEndpoint* ep, int tcp_fd) {
    // 1. Allocate CQ + QP
    if (ep->AllocateResources() < 0) return -1;

    // 2. Send HelloMessage
    HelloMessage local;
    local.block_size = g_rdma_recv_block_size;
    local.sq_size   = ep->sq_size_;
    local.rq_size   = ep->rq_size_;
    local.qp_num    = ep->qp_->qp_num;

    uint8_t data[HelloMessage::kMsgLen];
    local.Serialize(data);
    if (ep->WriteToFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    // 3. Receive remote HelloMessage
    if (ep->ReadFromFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;
    HelloMessage remote;
    remote.Deserialize(data);

    if (memcmp(remote.magic, "RDMA", 4) != 0) return -1;
    if (!HelloNegotiationValid(remote)) return -1;

    // 4. Set negotiated params
    ep->remote_recv_block_size_ = remote.block_size;
    ep->local_window_capacity_  = std::min<int>(ep->sq_size_, remote.rq_size) - RESERVED_WR_NUM;
    ep->remote_window_capacity_ = std::min<int>(ep->rq_size_, remote.sq_size) - RESERVED_WR_NUM;
    ep->sq_window_size_.store(ep->local_window_capacity_, std::memory_order_relaxed);
    ep->remote_rq_window_size_.store(ep->local_window_capacity_, std::memory_order_relaxed);
    ep->sq_imm_window_size_ = RESERVED_WR_NUM;

    // 5. Bring up QP (RESET->INIT->RTR->RTS)
    ibv_gid gid;
    memcpy(gid.raw, remote.gid, 16);
    if (ep->BringUpQp(remote.lid, gid, remote.qp_num) < 0) return -1;

    // 6. Send ACK (RDMA_OK)
    uint32_t ack = htonl(1);
    if (ep->WriteToFd(tcp_fd, &ack, 4) < 0) return -1;

    return 0;
}

int FastRdmaEndpoint::ProcessHandshakeAtServer(FastRdmaEndpoint* ep, int tcp_fd) {
    // 1. Read client HelloMessage
    uint8_t data[HelloMessage::kMsgLen];
    if (ep->ReadFromFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    HelloMessage remote;
    remote.Deserialize(data);
    if (memcmp(remote.magic, "RDMA", 4) != 0) return -1;
    if (!HelloNegotiationValid(remote)) return -1;

    // 2. Set negotiated params from client info
    ep->remote_recv_block_size_ = remote.block_size;
    ep->local_window_capacity_  = std::min<int>(ep->sq_size_, remote.rq_size) - RESERVED_WR_NUM;
    ep->remote_window_capacity_ = std::min<int>(ep->rq_size_, remote.sq_size) - RESERVED_WR_NUM;
    ep->sq_window_size_.store(ep->local_window_capacity_, std::memory_order_relaxed);
    ep->remote_rq_window_size_.store(ep->local_window_capacity_, std::memory_order_relaxed);
    ep->sq_imm_window_size_ = RESERVED_WR_NUM;

    // 3. Allocate QP/CQ
    if (ep->AllocateResources() < 0) return -1;

    // 4. Bring up QP (RESET→INIT→RTR→RTS), then QP is ready
    ibv_gid gid;
    memcpy(gid.raw, remote.gid, 16);
    if (ep->BringUpQp(remote.lid, gid, remote.qp_num) < 0) return -1;

    // 5. Send server HelloMessage (qp_num already available from AllocateResources)
    HelloMessage local;
    local.block_size = g_rdma_recv_block_size;
    local.sq_size   = ep->sq_size_;
    local.rq_size   = ep->rq_size_;
    local.qp_num    = ep->qp_ ? ep->qp_->qp_num : 0;

    local.Serialize(data);
    if (ep->WriteToFd(tcp_fd, data, HelloMessage::kMsgLen) < 0) return -1;

    // 6. Wait for client ACK
    uint32_t ack;
    if (ep->ReadFromFd(tcp_fd, &ack, 4) < 0) return -1;

    return 0;
}

// ---------------------------------------------------------------------------
// RDMA resource management
// ---------------------------------------------------------------------------

int FastRdmaEndpoint::AllocateResources() {
    comp_channel_ = ibv_create_comp_channel(g_ctx);
    CHECK(comp_channel_ != nullptr);

    send_cq_ = ibv_create_cq(g_ctx, sq_size_, nullptr, comp_channel_, 0);
    recv_cq_ = ibv_create_cq(g_ctx, rq_size_, nullptr, comp_channel_, 0);
    CHECK(send_cq_ != nullptr && recv_cq_ != nullptr);

    ibv_qp_init_attr qp_attr = {};
    qp_attr.send_cq = send_cq_;
    qp_attr.recv_cq = recv_cq_;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = sq_size_;
    qp_attr.cap.max_recv_wr  = rq_size_;
    qp_attr.cap.max_send_sge = g_rdma_max_sge;
    qp_attr.cap.max_recv_sge = 1;

    qp_ = ibv_create_qp(g_pd, &qp_attr);
    CHECK(qp_ != nullptr);

    sbuf_.resize(sq_size_ - RESERVED_WR_NUM);
    rbuf_.resize(rq_size_);
    rbuf_data_.resize(rq_size_, nullptr);

    ibv_req_notify_cq(send_cq_, 0);
    ibv_req_notify_cq(recv_cq_, 1);

    return 0;
}

int FastRdmaEndpoint::BringUpQp(uint16_t lid, ibv_gid gid, uint32_t remote_qpn) {
    ibv_qp_attr attr = {};

    // RESET -> INIT
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    CHECK(ibv_modify_qp(qp_, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) == 0);

    if (PostRecv(rq_size_, true) < 0) return -1;

    // INIT -> RTR
    memset(&attr, 0, sizeof(attr));
    attr.qp_state              = IBV_QPS_RTR;
    attr.path_mtu              = IBV_MTU_1024;
    attr.dest_qp_num           = remote_qpn;
    attr.rq_psn                = 0;
    attr.max_dest_rd_atomic    = 0;
    attr.min_rnr_timer         = 0;
    attr.ah_attr.dlid          = lid;
    attr.ah_attr.sl            = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.is_global     = 1;
    attr.ah_attr.port_num      = 1;
    attr.ah_attr.grh.dgid      = gid;
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit  = 16;
    CHECK(ibv_modify_qp(qp_, &attr,
        IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER | IBV_QP_AV) == 0);

    // RTR -> RTS
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout        = 14;
    attr.retry_cnt      = 7;
    attr.rnr_retry      = 0;
    attr.sq_psn         = 0;
    attr.max_rd_atomic  = 0;
    CHECK(ibv_modify_qp(qp_, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
        IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) == 0);

    return 0;
}

void FastRdmaEndpoint::DeallocateResources() {
    sbuf_.clear();
    rbuf_.clear();
    rbuf_data_.clear();

    if (qp_)      { ibv_destroy_qp(qp_);            qp_ = nullptr; }
    if (send_cq_) { ibv_destroy_cq(send_cq_);        send_cq_ = nullptr; }
    if (recv_cq_) { ibv_destroy_cq(recv_cq_);        recv_cq_ = nullptr; }
    if (comp_channel_) { ibv_destroy_comp_channel(comp_channel_); comp_channel_ = nullptr; }
}

// ---------------------------------------------------------------------------
// Recv
// ---------------------------------------------------------------------------

int FastRdmaEndpoint::DoPostRecv(void* block, size_t block_size) {
    ibv_recv_wr wr = {};
    ibv_sge sge = {};
    sge.addr   = reinterpret_cast<uint64_t>(block);
    sge.length  = static_cast<uint32_t>(block_size);
    sge.lkey    = GetRegionId(block);
    wr.sg_list  = &sge;
    wr.num_sge  = 1;

    ibv_recv_wr* bad = nullptr;
    return ibv_post_recv(qp_, &wr, &bad);
}

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
    new_rq_wrs_.fetch_add(num, std::memory_order_relaxed);
    return 0;
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

ssize_t FastRdmaEndpoint::CutFromIOBufList(IOBuf** from, size_t ndata) {
    uint32_t remote_rq_wnd = remote_rq_window_size_.load(std::memory_order_relaxed);
    uint32_t sq_wnd        = sq_window_size_.load(std::memory_order_relaxed);

    if (remote_rq_wnd == 0 || sq_wnd == 0) {
        errno = EAGAIN;
        return -1;
    }

    size_t   total_len = 0;
    size_t   current   = 0;
    ibv_send_wr wr;
    ibv_sge sglist[g_rdma_max_sge];

    while (current < ndata) {
        IOBuf* to = &sbuf_[sq_current_];
        size_t this_len = 0;

        memset(&wr, 0, sizeof(wr));
        wr.sg_list = sglist;
        wr.opcode  = IBV_WR_SEND_WITH_IMM;

        // ---- Cut IOBuf blocks into sge array ----
        size_t sge_index = 0;
        while (sge_index < static_cast<uint32_t>(g_rdma_max_sge) &&
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

        // ---- IMM data: carry recv credits ----
        uint32_t imm = new_rq_wrs_.exchange(0, std::memory_order_relaxed);
        wr.imm_data = htonl(imm);

        // ---- Solicited flag ----
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

        // ---- Selective signaling ----
        if (++sq_unsignaled_ >= local_window_capacity_ / 4) {
            wr.send_flags |= IBV_SEND_SIGNALED;
            wr.wr_id = sq_unsignaled_;
            sq_unsignaled_ = 0;
        }

        // ---- Post send ----
        ibv_send_wr* bad = nullptr;
        if (ibv_post_send(qp_, &wr, &bad) != 0) return -1;

        // ---- Advance ring buffer ----
        if (++sq_current_ == sbuf_.size()) sq_current_ = 0;

        // ---- Decrement windows ----
        remote_rq_wnd = remote_rq_window_size_.fetch_sub(1, std::memory_order_relaxed) - 1;
        sq_wnd        = sq_window_size_.fetch_sub(1, std::memory_order_relaxed) - 1;
    }

    return static_cast<ssize_t>(total_len);
}

// ---------------------------------------------------------------------------
// CQ
// ---------------------------------------------------------------------------

int FastRdmaEndpoint::comp_channel_fd() const {
    return comp_channel_ ? comp_channel_->fd : -1;
}

void FastRdmaEndpoint::PollCq(FastRdmaEndpoint* ep) {
    // TODO: ibv_poll_cq loop -> ep->HandleCompletion for each WC
}

ssize_t FastRdmaEndpoint::HandleCompletion(ibv_wc& wc) {
    bool zerocopy = true;
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
        if (remote_rq_window_size_.load(std::memory_order_relaxed) >= local_window_capacity_ / 8) {
            // Do not wake up writing thread right after polling IBV_WC_SEND.
            // Otherwise the writing thread may switch to background too quickly.
            send_cv_.notify_all();
        }
        return 0;

    case IBV_WC_RECV:
        if (wc.byte_len > 0) {
            if (wc.byte_len < g_rdma_zerocopy_min_size) {
                zerocopy = false;
            }
            if (zerocopy) {
                rbuf_[rq_received_].cutn(&read_buf_, wc.byte_len);
            } else {
                // Copy data when the receive data is really small
                read_buf_.append(rbuf_data_[rq_received_], wc.byte_len);
            }
        }
        if (0 != (wc.wc_flags & IBV_WC_WITH_IMM) && wc.imm_data > 0) {
            // Update window
            uint32_t acks = ntohs(wc.imm_data);
            uint32_t wnd_thresh = local_window_capacity_ / 8;
            uint32_t remote_rq_window_size =
                remote_rq_window_size_.fetch_add(acks, std::memory_order_relaxed);
            if (sq_window_size_.load(std::memory_order_relaxed) > 0 &&
                (remote_rq_window_size >= wnd_thresh || acks >= wnd_thresh)) {
                // Do not wake up writing thread right after _remote_rq_window_size > 0.
                // Otherwise the writing thread may switch to background too quickly.
                send_cv_.notify_all();
            }
        }
        // We must re-post recv WR
        if (PostRecv(1, zerocopy) < 0) {
            return -1;
        }
        if (wc.byte_len > 0) {
            SendAck(1);
        }
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
