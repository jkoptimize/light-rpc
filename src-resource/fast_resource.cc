#include "inc/fast_resource.h"
#include "inc/fast_log.h"

#include <arpa/inet.h>

namespace fast
{

  FastResource::FastResource(std::string local_ip, int local_port)
      : local_ip_(local_ip), local_port_(local_port)
  {
    BindToLocalRNIC();
  }

  FastResource::~FastResource()
  {
    auto temp_chan = cm_id_->channel;
    CHECK(rdma_destroy_id(cm_id_) == 0);
    rdma_destroy_event_channel(temp_chan);
  }

  void FastResource::BindToLocalRNIC()
  {
    auto temp_channel = rdma_create_event_channel();
    CHECK(temp_channel != nullptr);
    CHECK(rdma_create_id(temp_channel, &cm_id_, nullptr, RDMA_PS_TCP) == 0);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(local_ip_.c_str());
    addr.sin_port = htons(local_port_);
    CHECK(rdma_bind_addr(cm_id_, reinterpret_cast<sockaddr *>(&addr)) == 0);
    CHECK(cm_id_->verbs != nullptr && cm_id_->pd != nullptr);

    // Query device to get hardware max_send_sge.
    ibv_device_attr device_attr;
    CHECK(ibv_query_device(cm_id_->verbs, &device_attr) == 0);
    max_sge_ = device_attr.max_sge;
  }

} // namespace fast