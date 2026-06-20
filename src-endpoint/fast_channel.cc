#include <arpa/inet.h>

#include "fast_channel.h"
#include "fast_define.h"
#include "fast_iobuf.h"
#include "fast_log.h"
#include "message_dispatcher.h"
#include "build/fast_impl.pb.h"

namespace fast {

static const uint32_t kFixed32Bytes = 4;

// ============================================================
// FastChannel
// ============================================================

FastChannel::FastChannel(std::string dest_ip, int dest_port) {
    FastRdmaEndpoint::GlobalInitialize();
    endpoint_ = new FastRdmaEndpoint();
    endpoint_->SetRemoteAddr(dest_ip, dest_port);
    endpoint_->msg_dispatcher().SetMode(DispatcherMode::kClient);
    endpoint_->msg_dispatcher().SetHandler(OnProcessResponse, this);
}

FastChannel::~FastChannel() {
    // Wake up any blocked CallMethod threads.
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& kv : pending_map_) {
        PendingRequest* p = kv.second;
        std::lock_guard<std::mutex> lock2(p->mutex);
        p->done = true;
        p->cv.notify_one();
    }
    delete endpoint_;
}

void FastChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                              google::protobuf::RpcController* controller,
                              const google::protobuf::Message* request,
                              google::protobuf::Message* response,
                              google::protobuf::Closure* /*done*/) {
    // ---- 1. Build MetaDataOfRequest ----
    uint32_t rpc_id = rpc_id_.fetch_add(1, std::memory_order_relaxed);
    uint32_t attachment_len = request_attachment_.length();

    MetaDataOfRequest meta;
    meta.set_rpc_id(rpc_id);
    meta.set_service_name(method->service()->name());
    meta.set_method_name(method->name());
    meta.set_attachment_size(attachment_len);

    uint32_t meta_len    = meta.ByteSizeLong();
    uint32_t payload_len = request->ByteSizeLong();
    uint32_t total_len   = 2 * kFixed32Bytes + meta_len + payload_len + attachment_len;

    // ---- 2. Build IOBuf frame: [total_len(BE)][meta_len(BE)][meta][payload][attachment] ----
    // Frame header fields are in network byte order (big-endian), matching
    // CutInputMessage and OnProcessRequest which decode with ntohl.
    IOBuf frame;
    uint32_t be_total_len = htonl(total_len);
    uint32_t be_meta_len = htonl(meta_len);
    frame.append(&be_total_len, kFixed32Bytes);
    frame.append(&be_meta_len, kFixed32Bytes);

    {
        IOBufAsZeroCopyOutputStream zcos(&frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        meta.SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
    }
    {
        IOBufAsZeroCopyOutputStream zcos(&frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        request->SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
    }

    if (attachment_len > 0) {
        frame.append(request_attachment_);
    }
    request_attachment_.clear();

    // ---- 3. Register pending request ----
    PendingRequest pending;
    pending.response = response;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_map_[rpc_id] = &pending;
    }

    // ---- 4. Non-blocking enqueue ----
    if (endpoint_->StartWrite(std::move(frame)) < 0) {
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_map_.erase(rpc_id);
        }
        LOG_ERR("CallMethod: StartWrite failed, rpc_id=%u", rpc_id);
        if (controller) controller->SetFailed("StartWrite failed");
        return;
    }

    // ---- 5. Block waiting for response (1 min timeout) ----
    {
        std::unique_lock<std::mutex> lock(pending.mutex);
        bool ok = pending.cv.wait_for(lock, std::chrono::minutes(1),
                                      [&pending] { return pending.done; });
        if (!ok) {
            pending.timed_out = true;
            LOG_ERR("CallMethod timeout, rpc_id=%u", rpc_id);
            if (controller) controller->SetFailed("RPC timeout");
        }
    }

    if (!pending.timed_out) {
        if (pending.error_code != ErrorCode::ERR_SUCCESS) {
            if (controller) {
                switch (pending.error_code) {
                case ErrorCode::ERR_UNKNOWN_SERVICE:
                    controller->SetFailed("Server: unknown service"); break;
                case ErrorCode::ERR_UNKNOWN_METHOD:
                    controller->SetFailed("Server: unknown method"); break;
                case ErrorCode::ERR_BAD_REQUEST:
                    controller->SetFailed("Server: bad request"); break;
                case ErrorCode::ERR_BAD_RESPONSE:
                    controller->SetFailed("Client: response parse error"); break;
                case ErrorCode::ERR_INTERNAL:
                    controller->SetFailed("Server: internal error"); break;
                default:
                    controller->SetFailed("Unknown error"); break;
                }
            }
        } else {
            response_attachment_ = std::move(pending.attachment);
        }
    }

    // ---- 6. Cleanup ----
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_map_.erase(rpc_id);
    }
}

// ============================================================
// OnProcessResponse — called by MessageDispatcher detached thread
// ============================================================

int FastChannel::OnProcessResponse(IOBuf& frame, void* arg) {
    auto* self = static_cast<FastChannel*>(arg);

    // Frame format: [total_len:4B][rpc_id:4B][error_code:4B][attachment_size:4B][payload][attachment]
    // total_len was consumed by CutInputMessage; skip it.
    frame.pop_front(kFixed32Bytes);

    // Parse rpc_id (big-endian).
    uint32_t rpc_id = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(kFixed32Bytes);

    // Parse error_code.
    uint32_t error_code = ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(kFixed32Bytes);

    // Parse attachment_size.
    uint32_t attachment_size =
        ntohl(*static_cast<const uint32_t*>(frame.fetch1()));
    frame.pop_front(kFixed32Bytes);

    // Find pending request.
    std::lock_guard<std::mutex> lock(self->pending_mutex_);
    auto it = self->pending_map_.find(rpc_id);
    if (it == self->pending_map_.end()) {
        LOG_ERR("OnProcessResponse: unknown rpc_id=%u", rpc_id);
        return -1;
    }
    PendingRequest* pending = it->second;

    // Validate frame boundary.
    if (attachment_size > frame.length()) {
        LOG_ERR("OnProcessResponse: frame overflow, rpc_id=%u", rpc_id);
        pending->error_code = ErrorCode::ERR_BAD_RESPONSE;
        pending->done = true;
        pending->cv.notify_one();
        return -1;
    }
    uint32_t payload_len = frame.length() - attachment_size;

    // Server-reported error: skip payload parsing, notify immediately.
    if (error_code != ErrorCode::ERR_SUCCESS) {
        pending->error_code = error_code;
        pending->done = true;
        pending->cv.notify_one();
        return -1;
    }

    // Parse protobuf payload.
    {
        IOBufAsZeroCopyInputStream zcis(frame);
        if (!pending->response->ParseFromZeroCopyStream(&zcis)) {
            LOG_ERR("OnProcessResponse: failed to parse response, rpc_id=%u", rpc_id);
            pending->error_code = ErrorCode::ERR_BAD_RESPONSE;
            pending->done = true;
            pending->cv.notify_one();
            return -1;
        }
    }

    // Extract attachment (what remains after payload).
    frame.pop_front(payload_len);
    if (attachment_size > 0) {
        pending->attachment.append(frame);
    }

    {
        std::lock_guard<std::mutex> lock2(pending->mutex);
        pending->done = true;
    }
    pending->cv.notify_one();
    return 0;
}

}  // namespace fast
