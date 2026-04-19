#pragma once

#include "inc/fast_resource.h"
#include "inc/fast_utils.h"
#include "inc/fast_define.h"
#include "inc/fast_block_pool.h"

#include <rdma/rdma_cma.h>
#include <boost/asio.hpp>

namespace fast
{

  class SharedResource : public FastResource
  {
  public:
    SharedResource(std::string local_ip, int local_port,
                   int num_work_th = default_num_work_threads);
    virtual ~SharedResource();

    void PostOneRecvRequest(uint64_t block_addr);
    void CreateNewQueuePair(rdma_cm_id *conn_id);

    boost::asio::io_context *GetIOContext();
    int GetThreadIndex(std::thread::id th_id);
    uint32_t GetLocalKey() const;
    uint32_t GetRemoteKey() const;

  private:
    virtual void CreateRDMAResource() override;
    void InitBlockPool();

    int num_work_threads_;
    std::atomic<uint64_t> schedule_idx_;

    std::vector<std::thread> threads_vec_;
    std::unordered_map<std::thread::id, int> thid_idx_map_;
    std::vector<std::unique_ptr<boost::asio::io_context>> io_ctx_vec_;

    static const int default_num_work_threads;
  };

  inline boost::asio::io_context *SharedResource::GetIOContext()
  {
    int idx = ++schedule_idx_ % num_work_threads_;
    return io_ctx_vec_.at(idx).get();
  }

  inline int SharedResource::GetThreadIndex(std::thread::id th_id)
  {
    return thid_idx_map_.at(th_id);
  }

  // Note: GetLocalKey/GetRemoteKey are deprecated for non-large-message paths.
  // Use GetRegionId(block_addr) per block instead.
  inline uint32_t SharedResource::GetLocalKey() const { return 0; }
  inline uint32_t SharedResource::GetRemoteKey() const { return 0; }

} // namespace fast
