#pragma once
#include "fast_rdma_endpoint.h"

namespace fast {
// Test fixture ownership follows the production Create/Address/SetFailed
// protocol; no stack-allocated or directly deleted pooled endpoint.
class EndpointTestOwner {
public:
    EndpointTestOwner() {
        CHECK_EQ(0, FastRdmaEndpoint::Create(&id_));
        CHECK_EQ(0, FastRdmaEndpoint::Address(id_, &reference_));
    }
    ~EndpointTestOwner() { if (reference_) reference_->SetFailed(); }
    FastRdmaEndpoint& get() { return *reference_; }
    EndpointUniquePtr release() { return std::move(reference_); }
private:
    EndpointId id_;
    EndpointUniquePtr reference_;
};
}
