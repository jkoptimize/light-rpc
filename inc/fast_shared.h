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

    /// @brief Returns the device's max_send_sge (hardware SGE limit for scatter-gather send).
    int max_send_sge() const { return max_send_sge_; }

    /**
     * @brief Allocate a temporarily-registered large memory block for large messages.
     *   First searches the TLS big-block cache for a best-fit block, then falls back
     *   to malloc + ibv_reg_mr. Allocated blocks must be returned via ReturnLargeBlock().
     * @param size Required minimum size (bytes). Must be > 0.
     * @return Pointer to the registered ibv_mr, or nullptr on failure.
     * @note Caller retrieves lkey via mr->lkey.
     */
    ibv_mr *LargeBlockAlloc(size_t size);

    /**
     * @brief Return a large block to the TLS big-block cache or deregister if no fit.
     * @param mr Memory region pointer returned by LargeBlockAlloc().
     */
    void ReturnLargeBlock(ibv_mr *mr);

    void ObtainOneBlock(uint64_t &block_addr);
    void ReturnOneBlock(uint64_t &block_addr);

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

  inline void SharedResource::ObtainOneBlock(uint64_t &block_addr)
  {
    block_addr = reinterpret_cast<uint64_t>(BlockAllocate(msg_threshold));
  }

  inline void SharedResource::ReturnOneBlock(uint64_t &block_addr)
  {
    BlockDeallocate(reinterpret_cast<void *>(block_addr));
  }

} // namespace fast
