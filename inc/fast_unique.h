#pragma once

#include "inc/fast_resource.h"
#include "inc/fast_block_pool.h"
#include "inc/fast_define.h"

#include <rdma/rdma_cma.h>

namespace fast
{

  class UniqueResource : public FastResource
  {
  public:
    UniqueResource(std::string local_ip, int local_port = 0);
    virtual ~UniqueResource();

    void PostOneRecvRequest(uint64_t &block_addr);
    void ObtainOneBlock(uint64_t &block_addr);
    void ReturnOneBlock(uint64_t &block_addr);
    uint32_t GetLocalKey() const;
    uint32_t GetRemoteKey() const;

  private:
    virtual void CreateRDMAResource() override;
    void InitBlockPoolWithCb();

    uint32_t lkey_;
    uint32_t rkey_;
  };

  inline void UniqueResource::ObtainOneBlock(uint64_t &block_addr)
  {
    block_addr = reinterpret_cast<uint64_t>(BlockAllocate(msg_threshold));
  }

  inline void UniqueResource::ReturnOneBlock(uint64_t &block_addr)
  {
    BlockDeallocate(reinterpret_cast<void *>(block_addr));
  }

  inline uint32_t UniqueResource::GetLocalKey() const
  {
    return lkey_;
  }

  inline uint32_t UniqueResource::GetRemoteKey() const
  {
    return rkey_;
  }

} // namespace fast
