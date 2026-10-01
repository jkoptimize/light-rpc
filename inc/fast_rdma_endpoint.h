#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <infiniband/verbs.h>
#include "fast_iobuf.h"
#include "message_dispatcher.h"
#include "versioned_ref_with_id.h"
#include "event_dispatcher.h"

namespace fast {

class FastServer;  // forward declare
class FastRdmaEndpoint;
using EndpointId = VRefId;
using EndpointUniquePtr = VersionedRefWithIdUniquePtr<FastRdmaEndpoint>;

// ============================================================
// HelloMessage — 44B TCP 带外握手消息
// ============================================================

struct HelloMessage {
    static const size_t   kMsgLen = 44;
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
    ibv_gid  gid = {};
    uint32_t qp_num = 0;
    uint32_t data_qp_num = 0;

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

class FastRdmaEndpoint : public VersionedRefWithId<FastRdmaEndpoint> {
    static const int PROGRESS_INIT = 1;
public:
    explicit FastRdmaEndpoint(Forbidden f);
    ~FastRdmaEndpoint() override;
    // Caller must own a reference. Failure never waits for the calling worker.
    int SetFailed(int error = ECANCELED) {
        return VersionedRefWithId<FastRdmaEndpoint>::SetFailed(error ? error : EIO);
    }
    int error() const {
        const int value = _write_error.load(std::memory_order_acquire);
        return value ? value : (Failed() ? ECANCELED : 0);
    }
    // Install owner callbacks before publishing the endpoint to other threads.
    void SetFailureHandler(std::function<void(int)> handler) { _failure_handler = std::move(handler); }
    void SetRecycleHandler(std::function<void()> handler) { _recycle_handler = std::move(handler); }

    // ---- Global init (call once before any endpoint is created) ----
    static void GlobalInitialize();

    // ============ Handshake (static — ep passed as arg, ref brpc) ============
    static int ProcessHandshakeAtClient(FastRdmaEndpoint* ep, int tcp_fd);
    static int ProcessHandshakeAtServer(FastRdmaEndpoint* ep, int tcp_fd);

    // ============ QP Resource Management ============
    static constexpr int kDataQpDepth = 8;
    static constexpr int kMaxLargeTransfers = 8;

    // ============ Large Transfer ============
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
    void StoreLargeFrame(uint32_t rpc_id, IOBuf&& frame);
    IOBuf* GetLargeFrame(uint32_t rpc_id);
    void ReleaseLargeFrame(uint32_t rpc_id);
    int PostLargeWriteRecv(ibv_mr* mr);
    ssize_t CutSegFromIOBuf(IOBuf* buf, uint32_t remote_rkey, uint64_t remote_addr,
                             uint32_t imm_rkey, uint32_t rpc_id);

    int AllocateResources();
    int BringUpQp(uint16_t lid, ibv_gid gid, uint32_t remote_qpn);
    int BringUpDataQp(uint16_t lid, ibv_gid gid, uint32_t remote_qpn);
    void DeallocateResources();

    // ============ Send (called by KeepWrite thread) ============
    ssize_t CutFromIOBufList(IOBuf** from, size_t ndata);
    int StartWrite(IOBuf&& data);
    bool IsWritable() const;
    void WaitForWritable();

    // ---- Connection ----
    void SetRemoteAddr(const std::string& ip, int port);
    bool IsHandshakeOk() const { return _handshake_ok.load(std::memory_order_acquire); }

    // ============ Recv & CQ (called by Poller thread) ============
    static void PollCq(FastRdmaEndpoint* ep);
    int GetAndAckEvents();
    // ---------- EventDispatcher callbacks ----------

    // Server: listen fd readable — accept connection
    static void OnServerAccept(void* user_data, uint32_t events);

    // Server: client_fd readable — read HelloMessage, start handshake
    static void OnServerHandshake(void* user_data, uint32_t events);

    // Client: sock_fd writable (connect complete) — start handshake
    static void OnClientHandshake(void* user_data, uint32_t events);

    // comp_channel fd readable — poll CQ
    static void OnCompChannelEvent(void* user_data, uint32_t events);

    ssize_t HandleCompletion(ibv_wc& wc);
    int PostRecv(uint32_t num, bool zerocopy);

    // ============ Query ============
    ibv_qp* qp() const { return qp_; }
    int comp_channel_fd() const;
    MessageDispatcher& msg_dispatcher() { return _msg_dispatcher; }
    FastServer* owner() const { return _owner; }

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
    friend class VersionedRefWithId<FastRdmaEndpoint>;
    int OnCreated();
    void OnFailed(int error);
    void BeforeRecycled();
    // For unit tests only: exercise event ownership without RDMA resources.
    friend class FastRdmaEndpointEventTestPeer;
    // For unit tests only: validate fd setup without RDMA resources.
    friend class FastRdmaEndpointFdTestPeer;
    // For unit tests only: exercise the write queue without RDMA resources.
    friend class FastRdmaEndpointWriteTestPeer;
    // For unit tests only: exercise shutdown using ordinary Linux sockets.
    friend class FastRdmaEndpointLifecycleTestPeer;
    // For unit tests only: verify window waits without RDMA resources.
    friend class FastRdmaEndpointWritableTestPeer;

    int SendAck(int num);
    int SendImm(uint32_t imm);
    int DoPostRecv(void* block, size_t block_size);
    int ReadFromFd(int fd, void* data, size_t len);
    int WriteToFd(int fd, const void* data, size_t len);
    static int SetNonBlocking(int fd);
    bool AddReadEvent();
    bool MoreReadEvents(int* progress);
    void StopCqPolling();
    void CloseTcpFd();
    int RegisterTcpEvent(int fd, EventDispatcher::InputCallback cb, uint32_t events);
    void UnregisterTcpEvent();

    // ---- Write queue (ref brpc Socket::StartWrite / KeepWrite / IsWriteComplete) ----
    struct WriteRequest {
        static WriteRequest* const UNCONNECTED;
        IOBuf         data;
        WriteRequest* next = UNCONNECTED;
    };
    WriteRequest* PublishWriteRequest(WriteRequest* req);
    int WriteError() const;
    int FailWrite(WriteRequest* req, int error);
    void ReleaseAllFailedWriteRequests(WriteRequest* req);
    void FailPendingWrite(int error);
    int StartKeepWrite(WriteRequest* req);
    void KeepWrite(WriteRequest* req);
    ssize_t DoWrite(WriteRequest* req);
    void WakeForWritable();
    bool IsWriteComplete(WriteRequest* old_head, bool singular,
                         WriteRequest** new_tail);
    int StartAsyncConnect();

    // ---- RDMA resources ----
    ibv_qp*            qp_ = nullptr;
    ibv_cq*            send_cq_ = nullptr;
    ibv_cq*            recv_cq_ = nullptr;
    ibv_qp*            data_qp_ = nullptr;
    ibv_cq*            data_send_cq_ = nullptr;
    ibv_cq*            data_recv_cq_ = nullptr;
    ibv_comp_channel*  comp_channel_ = nullptr;

    // ---- TCP fd used during handshake ----
    int tcp_fd_ = -1;
    std::mutex connection_mutex_;
    EventDispatcher::Registration tcp_registration_ = EventDispatcher::INVALID_REGISTRATION;
    EventDispatcher::Registration cq_registration_ = EventDispatcher::INVALID_REGISTRATION;
    int cq_event_fd_ = -1;

    // ---- Server-side owner (nullptr for client) ----
    FastServer* _owner = nullptr;

    // ---- Negotiated params ----
    uint16_t sq_size_{128};
    uint16_t rq_size_{128};
    uint32_t remote_recv_block_size_{0};
    int      local_window_capacity_{0};
    int      remote_window_capacity_{0};

    // ---- Flow control ----
    std::atomic<int> sq_window_size_{0};
    std::atomic<int> remote_rq_window_size_{0};
    std::atomic<int>* writable_butex_{nullptr};
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

    // ---- ReadBuffer + message dispatching ----
    IOBuf              read_buf_;
    MessageDispatcher  _msg_dispatcher;

    // ---- PollCq event counter (ref brpc Socket::_nevent) ----
    std::atomic<int>  _nevent{0};

    // ---- Write queue ----
    std::atomic<WriteRequest*> _write_head{nullptr};
    std::atomic<bool>         _handshake_ok{false};
    std::atomic<WriteRequest*> _pending_keepwrite_req{nullptr};
    std::atomic<int>          _write_error{0};
    std::string               _remote_ip;
    int                       _remote_port{0};

    // ---- Selective signaling stats ----
    int send_counter_{0};
    int sq_unsignaled_{0};
    int unsolicited_{0};
    int accumulated_ack_{0};

    // ---- CQ event counters (ref brpc RdmaEndpoint) ----
    int send_cq_events{0};
    int recv_cq_events{0};
    int data_send_cq_events{0};
    int data_recv_cq_events{0};

    // ---- Large transfer flow control ----
    std::atomic<int> active_large_transfers_{0};

    // ---- Large frame storage ----
    std::mutex large_frame_mutex_;
    std::unordered_map<uint32_t, IOBuf> pending_large_frames_;

public:
    // ---- LargeBlock tracking (FastChannel::OnProcessResponse needs access) ----
    std::mutex pending_large_mutex_;
    std::unordered_map<uint32_t, ibv_mr*> pending_large_map_;  // key = rkey

private:
    // ---- Shutdown ----
    std::atomic<bool> _stop{false};
    std::function<void(int)> _failure_handler;
    std::function<void()> _recycle_handler;
};

}  // namespace fast
