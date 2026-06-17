#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include "fast_iobuf.h"
#include "fast_rdma_endpoint.h"

namespace fast {

enum ServiceOwnership {
    SERVER_OWNS_SERVICE,
    SERVER_DOESNT_OWN_SERVICE
};

struct ServiceInfo {
    ServiceOwnership ownership;
    google::protobuf::Service* service;
};

struct CallBackArgs {
    uint32_t            rpc_id;
    FastRdmaEndpoint*   endpoint;
    google::protobuf::Message* request;
    google::protobuf::Message* response;
    IOBuf               request_attachment;
    IOBuf               response_attachment;
};

class FastServer {
public:
    FastServer(std::string local_ip, int local_port);
    ~FastServer();

    void AddService(ServiceOwnership ownership,
                    google::protobuf::Service* service);
    void BuildAndStart();
    int  listen_fd() const { return listen_fd_; }

    void AddEndpoint(uint32_t qp_num, FastRdmaEndpoint* ep);

    static int OnProcessRequest(IOBuf& frame, void* arg);

private:
    void ReturnRPCResponse(CallBackArgs args);

    std::string local_ip_;
    int         local_port_;
    int         listen_fd_{-1};

    std::mutex conn_mutex_;
    std::unordered_map<uint32_t, FastRdmaEndpoint*> conn_map_;

    std::unordered_map<std::string, ServiceInfo> service_map_;

    // Deprecated — kept for future use
    // int num_pollers_;
    // std::vector<std::thread> poller_pool_;
    // volatile std::atomic<bool> stop_flag_;
};

}  // namespace fast
