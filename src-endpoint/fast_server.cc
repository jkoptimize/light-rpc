#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "event_dispatcher.h"
#include "fast_define.h"
#include "fast_large_block.h"
#include "fast_log.h"
#include "fast_server.h"
#include "build/fast_impl.pb.h"

namespace fast {

// ============================================================
// FastServer
// ============================================================

FastServer::FastServer(std::string local_ip, int local_port)
    : local_ip_(std::move(local_ip)), local_port_(local_port) {}

FastServer::~FastServer() {
    {
        std::lock_guard<std::mutex> lock(large_mutex_);
        closed_ = true;
    }
    large_cv_.notify_all();

    std::lock_guard<std::mutex> lock(conn_mutex_);
    for (auto& kv : conn_map_) {
        delete kv.second;
    }
}

void FastServer::AddService(ServiceOwnership ownership,
                             google::protobuf::Service* service) {
    std::string name(service->GetDescriptor()->name());
    CHECK(service_map_.find(name) == service_map_.end());
    ServiceInfo info;
    info.ownership = ownership;
    info.service   = service;
    service_map_.emplace(name, info);
}

void FastServer::BuildAndStart() {
    FastRdmaEndpoint::GlobalInitialize();
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd_ >= 0);

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(local_port_));
    addr.sin_addr.s_addr = inet_addr(local_ip_.c_str());

    CHECK(bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) == 0);
    CHECK(listen(listen_fd_, 200) == 0);

    fcntl(listen_fd_, F_SETFL, O_NONBLOCK);
    fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);

    EventDispatcher::GetInstance().RegisterEvent(
        listen_fd_,
        FastRdmaEndpoint::OnServerAccept,
        nullptr,   // output_cb
        this,      // user_data = FastServer*
        EPOLLIN | EPOLLET);
}

void FastServer::AddEndpoint(uint32_t qp_num, FastRdmaEndpoint* ep) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    conn_map_[qp_num] = ep;
}

// ============================================================
// OnProcessRequest — static handler, called by MessageDispatcher
// ============================================================

int FastServer::OnProcessRequest(IOBuf& frame, void* arg) {
    auto* ep     = static_cast<FastRdmaEndpoint*>(arg);
    auto* server = ep->owner();
    if (server == nullptr) return -1;

    // Frame: [total_len:4B][msg_type:4B][rpc_id:4B][...]
    // total_len already consumed by CutInputMessage; skip it.
    uint32_t total_len = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    uint32_t msg_type = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    uint32_t rpc_id = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    // === MSG_NOTIFY: Client has a Large request ===
    if (msg_type == MSG_NOTIFY) {
        uint32_t data_total_len = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
        ibv_mr* mr = LargeBlockAlloc(data_total_len);
        if (mr == nullptr) return -1;
        ep->PostLargeWriteRecv(mr);

        IOBuf auth_frame;
        uint32_t be;
        be = htonl(kAuthFrameBytes);     auth_frame.append(&be, 4);
        be = htonl(MSG_AUTHORITY);        auth_frame.append(&be, 4);
        be = htonl(rpc_id);               auth_frame.append(&be, 4);
        be = htonl(mr->rkey);             auth_frame.append(&be, 4);
        uint64_t addr = reinterpret_cast<uint64_t>(mr->addr);
        be = htonl(static_cast<uint32_t>(addr >> 32));
        auth_frame.append(&be, 4);
        be = htonl(static_cast<uint32_t>(addr & 0xFFFFFFFF));
        auth_frame.append(&be, 4);
        ep->StartWrite(std::move(auth_frame));

        {
            std::lock_guard<std::mutex> lock(ep->pending_large_mutex_);
            ep->pending_large_map_[mr->rkey] = mr;
        }
        return 0;
    }

    // === MSG_AUTHORITY: Client replied with LargeBlock address (server sends Large response) ===
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

    // === MSG_NORMAL: existing logic ===
    // After consuming total_len + msg_type + rpc_id, frame starts at meta_len.
    uint32_t meta_len = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(4);

    // Parse MetaDataOfRequest.
    MetaDataOfRequest meta;
    {
        IOBuf meta_buf;
        frame.cutn(&meta_buf, meta_len);
        IOBufAsZeroCopyInputStream zcis(meta_buf);
        if (!meta.ParseFromZeroCopyStream(&zcis)) {
            LOG_ERR("OnProcessRequest: failed to parse meta");
            return -1;  // rpc_id unavailable — client will timeout
        }
    }

    std::string service_name = meta.service_name();
    std::string method_name  = meta.method_name();
    uint32_t    attachment_size = meta.attachment_size();

    // Validate frame boundaries.
    // Header: total_len(4) + msg_type(4) + rpc_id(4) + meta_len(4) + meta(meta_len) = 16 + meta_len
    uint32_t header_size = 16 + meta_len;
    if (total_len < header_size || attachment_size > total_len - header_size) {
        LOG_ERR("OnProcessRequest: frame overflow, rpc_id=%u", rpc_id);
        SendErrorResponse(ep, rpc_id, ERR_BAD_REQUEST);
        return -1;
    }
    uint32_t payload_len = total_len - header_size - attachment_size;

    // Look up service.
    auto svc_iter = server->service_map_.find(service_name);
    if (svc_iter == server->service_map_.end()) {
        LOG_ERR("OnProcessRequest: unknown service %s", service_name.c_str());
        SendErrorResponse(ep, rpc_id, ERR_UNKNOWN_SERVICE);
        return -1;
    }
    auto* service = svc_iter->second.service;
    auto* method = service->GetDescriptor()->FindMethodByName(method_name);
    if (method == nullptr) {
        LOG_ERR("OnProcessRequest: unknown method %s", method_name.c_str());
        SendErrorResponse(ep, rpc_id, ERR_UNKNOWN_METHOD);
        return -1;
    }

    // Create request/response.
    std::unique_ptr<google::protobuf::Message> request(
        service->GetRequestPrototype(method).New());
    std::unique_ptr<google::protobuf::Message> response(
        service->GetResponsePrototype(method).New());

    // Parse payload.
    {
        IOBuf payload_buf;
        frame.cutn(&payload_buf, payload_len);
        IOBufAsZeroCopyInputStream zcis(payload_buf);
        if (!request->ParseFromZeroCopyStream(&zcis)) {
            LOG_ERR("OnProcessRequest: failed to parse request, rpc_id=%u", rpc_id);
            SendErrorResponse(ep, rpc_id, ERR_BAD_REQUEST);
            return -1;
        }
    }

    // Extract attachment.
    IOBuf req_attachment;
    if (attachment_size > 0) {
        req_attachment.append(frame);  // remaining in frame is attachment
    }

    // Dispatch.
    CallBackArgs args;
    args.rpc_id    = rpc_id;
    args.error_code = ERR_SUCCESS;
    args.endpoint  = ep;
    args.request   = request.release();
    args.response  = response.release();
    args.request_attachment = std::move(req_attachment);

    auto* done = google::protobuf::NewCallback(
        server, &FastServer::ReturnRPCResponse, args);
    service->CallMethod(method, nullptr, args.request, args.response, done);
    return 0;
}

// ============================================================
// ReturnRPCResponse — called when service done
// ============================================================

void FastServer::SendErrorResponse(FastRdmaEndpoint* ep, uint32_t rpc_id,
                                     ErrorCode error_code) {
    // Minimal frame with error_code, no payload, no attachment.
    // Frame: [total_len(BE)][msg_type(BE)][rpc_id(BE)][error_code(BE)][attachment_size=0(BE)]
    const uint32_t total_len = 20;
    IOBuf frame;
    uint32_t be;
    be = htonl(total_len);           frame.append(&be, 4);
    be = htonl(MSG_NORMAL_RESPONSE); frame.append(&be, 4);
    be = htonl(rpc_id);              frame.append(&be, 4);
    be = htonl(error_code);          frame.append(&be, 4);
    be = 0;                          frame.append(&be, 4);  // attachment_size = 0
    ep->StartWrite(std::move(frame));
}

void FastServer::WaitForLargeWritable(FastRdmaEndpoint* ep) {
    std::unique_lock<std::mutex> lock(large_mutex_);
    large_cv_.wait(lock, [this, ep] {
        return ep->CanStartLargeTransfer() || closed_;
    });
    if (!closed_) ep->StartLargeTransfer();
}

void FastServer::ReturnRPCResponse(CallBackArgs args) {
    std::unique_ptr<google::protobuf::Message> req_guard(args.request);
    std::unique_ptr<google::protobuf::Message> resp_guard(args.response);

    uint32_t attachment_len = args.response_attachment.length();
    // On error (error_code != 0), response and request may be nullptr.
    uint32_t payload_len = (args.response != nullptr)
                               ? args.response->ByteSizeLong() : 0;
    uint32_t total_len   = 20 + payload_len + attachment_len;

    // Frame: [total_len(BE)][msg_type(BE)][rpc_id(BE)][error_code(BE)][attachment_size(BE)][payload][attachment]
    IOBuf frame;
    uint32_t be;

    be = htonl(total_len);               frame.append(&be, 4);
    be = htonl(MSG_NORMAL_RESPONSE);     frame.append(&be, 4);
    be = htonl(args.rpc_id);             frame.append(&be, 4);
    be = htonl(args.error_code);         frame.append(&be, 4);
    be = htonl(attachment_len);          frame.append(&be, 4);

    if (args.response != nullptr) {
        IOBufAsZeroCopyOutputStream zcos(&frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        args.response->SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
    }

    if (attachment_len > 0) {
        frame.append(args.response_attachment);
    }

    if (total_len >= msg_threshold) {
        // Large path: store frame, send MSG_NOTIFY on control QP
        args.endpoint->StoreLargeFrame(args.rpc_id, std::move(frame));
        WaitForLargeWritable(args.endpoint);

        IOBuf notify_frame;
        be = htonl(kNotifyFrameBytes);  notify_frame.append(&be, 4);
        be = htonl(MSG_NOTIFY);          notify_frame.append(&be, 4);
        be = htonl(args.rpc_id);         notify_frame.append(&be, 4);
        be = htonl(total_len);           notify_frame.append(&be, 4);
        args.endpoint->StartWrite(std::move(notify_frame));
        // MSG_AUTHORITY arrives -> OnProcessRequest triggers CutSegFromIOBuf
        return;
    }

    // Medium path
    args.endpoint->StartWrite(std::move(frame));
}

}  // namespace fast
