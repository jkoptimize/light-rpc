#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>

#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include "fast_iobuf.h"
#include "fast_rdma_endpoint.h"

namespace fast {

class FastChannel : public google::protobuf::RpcChannel {
public:
    FastChannel(std::string dest_ip, int dest_port);
    ~FastChannel() override;

    void CallMethod(const google::protobuf::MethodDescriptor* method,
                    google::protobuf::RpcController* controller,
                    const google::protobuf::Message* request,
                    google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

    void SetRequestAttachment(IOBuf&& attachment) {
        request_attachment_ = std::move(attachment);
    }
    void SetRequestAttachment(const void* data, size_t len) {
        request_attachment_.clear();
        request_attachment_.append(data, len);
    }
    const IOBuf& ResponseAttachment() const { return response_attachment_; }

private:
    static int OnProcessResponse(IOBuf& frame, void* arg);

    FastRdmaEndpoint* endpoint_;

    uint32_t rpc_id_{1};
    IOBuf request_attachment_;
    IOBuf response_attachment_;

    struct PendingRequest {
        std::mutex              mutex;
        std::condition_variable cv;
        google::protobuf::Message* response   = nullptr;
        bool                    done         = false;
        bool                    timed_out    = false;
        uint32_t                error_code   = 0;
        IOBuf                   attachment;
    };

    std::mutex pending_mutex_;
    std::unordered_map<uint32_t, PendingRequest*> pending_map_;
};

}  // namespace fast
