#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <infiniband/verbs.h>
#include "fast_iobuf.h"

namespace fast {

// ============================================================
// HelloMessage — 40B TCP 带外握手消息
// ============================================================

struct HelloMessage {
    static const size_t   kMsgLen = 40;
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

// ============================================================
// RdmaIOBuf — IOBuf subclass with RDMA sge-cutting capability
// ============================================================

class RdmaIOBuf : public IOBuf {
    friend class FastRdmaEndpoint;

public:
    static const size_t IOBUF_BLOCK_HEADER_LEN = 32;

private:
    // Cut blocks from this IOBuf into ibv_sge array, moving data to `to`.
    ssize_t cut_into_sglist_and_iobuf(ibv_sge* sglist, size_t* sge_index,
                                       IOBuf* to, size_t max_sge, size_t max_len);
};

// ============================================================
// FastRdmaEndpoint — per-connection RDMA endpoint
// ============================================================

class FastRdmaEndpoint {
public:
    FastRdmaEndpoint();
    ~FastRdmaEndpoint();

    // ---- Global init (call once before any endpoint is created) ----
    static void GlobalInitialize();

    // ============ Handshake (static — ep passed as arg, ref brpc) ============
    static int ProcessHandshakeAtClient(FastRdmaEndpoint* ep, int tcp_fd);
    static int ProcessHandshakeAtServer(FastRdmaEndpoint* ep, int tcp_fd);

    // ============ QP Resource Management ============
    int AllocateResources();
    int BringUpQp(uint16_t lid, ibv_gid gid, uint32_t remote_qpn);
    void DeallocateResources();

    // ============ Send (called by KeepWrite thread) ============
    ssize_t CutFromIOBufList(IOBuf** from, size_t ndata);
    bool IsWritable() const;
    void WaitForWritable();

    // ============ Recv & CQ (called by Poller thread) ============
    static void PollCq(FastRdmaEndpoint* ep);
    ssize_t HandleCompletion(ibv_wc& wc);
    int PostRecv(uint32_t num, bool zerocopy);

    // ============ Query ============
    ibv_qp* qp() const { return qp_; }
    int comp_channel_fd() const;

    // ============ Test Helpers ============
    // These exist solely for unit tests to set up flow-control state
    // without a full handshake.  Do NOT use in production code paths.
    int sq_window_size() const {
        return sq_window_size_.load(std::memory_order_relaxed);
    }
    int remote_rq_window_size() const {
        return remote_rq_window_size_.load(std::memory_order_relaxed);
    }
    int new_rq_wrs() const {
        return new_rq_wrs_.load(std::memory_order_relaxed);
    }
    void SetNegotiatedParams(uint16_t sq_size, uint16_t rq_size,
                              uint16_t remote_sq_size, uint16_t remote_rq_size,
                              uint32_t block_size);
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
    size_t              sq_unsignaled{0};
    std::vector<IOBuf>  rbuf_;
    std::vector<void*>  rbuf_data_;
    size_t              rq_received_{0};

    // ---- ReadBuffer for incoming data ----
    IOBuf              read_buf_;

    // ---- Blocking wait ----
    std::mutex              send_mutex_;
    std::condition_variable send_cv_;

    // ---- Selective signaling stats ----
    int send_counter_{0};
    int sq_unsignaled_{0};
    int unsolicited_{0};
    int accumulated_ack_{0};
};

}  // namespace fast
