#include "inc/fast_unique.h"
#include "inc/fast_define.h"
#include "inc/fast_log.h"
#include "inc/fast_utils.h"

#include <mutex>

static const int min_cqe_num = 32;
static const uint32_t max_recv_wr = 32; // 用户可配置
static const uint32_t max_send_wr = 32; // 用户可配置

static std::once_flag init_block_pool_flag;

namespace fast
{

  struct LargeBlockNode
  {
    void *buf;
    size_t size;
    ibv_mr *mr; // pointer to registered mr, needed for dereg_mr
    uint32_t lkey;
    LargeBlockNode *next;
  };

  // TLS big-block cache: best-fit search, max 8 cached blocks.
  static const size_t kMaxCachedLargeBlocks = 8;
  static __thread LargeBlockNode *tls_large_block_list = nullptr;
  static __thread size_t tls_large_block_num = 0;

  static __thread bool tls_large_inited = false;

  void RecycleTLSLargeBlocks()
  {
    while (tls_large_block_list != nullptr)
    {
      LargeBlockNode *node = tls_large_block_list;
      tls_large_block_list = node->next;
      CHECK(ibv_dereg_mr(node->mr) == 0);
      free(node->buf);
      delete node;
    }
    tls_large_block_num = 0;
  }

  void UniqueResource::PostOneRecvRequest(uint64_t &block_addr)
  {
    ObtainOneBlock(block_addr);
    ibv_sge recv_sg;
    recv_sg.addr = block_addr;
    recv_sg.length = default_msg_size;
    recv_sg.lkey = ::fast::GetRegionId(reinterpret_cast<void *>(block_addr));

    ibv_recv_wr recv_wr;
    ibv_recv_wr *recv_bad_wr = nullptr;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = block_addr;
    recv_wr.num_sge = 1;
    recv_wr.sg_list = &recv_sg;
    CHECK(ibv_post_recv(cm_id_->qp, &recv_wr, &recv_bad_wr) == 0);
  }

  void UniqueResource::CreateRDMAResource()
  {
    cm_id_->recv_cq_channel = ibv_create_comp_channel(cm_id_->verbs);
    CHECK(cm_id_->recv_cq_channel != nullptr);
    cm_id_->send_cq_channel = ibv_create_comp_channel(cm_id_->verbs);
    CHECK(cm_id_->send_cq_channel != nullptr);
    cm_id_->recv_cq = ibv_create_cq(
        cm_id_->verbs, min_cqe_num, nullptr, cm_id_->recv_cq_channel, 0);
    CHECK(cm_id_->recv_cq != nullptr);
    cm_id_->send_cq = ibv_create_cq(
        cm_id_->verbs, min_cqe_num, nullptr, cm_id_->send_cq_channel, 0);
    CHECK(cm_id_->send_cq != nullptr);

    ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.sq_sig_all = 0;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.recv_cq = cm_id_->recv_cq;
    qp_attr.send_cq = cm_id_->send_cq;
    qp_attr.cap.max_recv_wr = max_recv_wr;
    qp_attr.cap.max_recv_sge = max_send_sge();
    qp_attr.cap.max_send_wr = max_send_wr;
    qp_attr.cap.max_send_sge = max_send_sge();
    qp_attr.cap.max_inline_data = max_inline_data;
    CHECK(rdma_create_qp(cm_id_, cm_id_->pd, &qp_attr) == 0);
  }

  void UniqueResource::InitBlockPool()
  {
    std::call_once(init_block_pool_flag, [this]()
                   {
                     ::fast::SetGlobalPD(cm_id_->pd);
                     ::fast::InitBlockPool(); });
  }

  ibv_mr *UniqueResource::LargeBlockAlloc(size_t size)
  {
    if (!tls_large_inited)
    {
      tls_large_inited = true;
      ThreadExitHelper::add_callback([]
                                     { RecycleTLSLargeBlocks(); });
    }

    // Best-fit search in TLS cache.
    LargeBlockNode *best_prev = nullptr;
    LargeBlockNode *prev = nullptr;
    LargeBlockNode *cur = tls_large_block_list;
    size_t best_diff = SIZE_MAX;

    while (cur != nullptr)
    {
      if (cur->size >= size)
      {
        size_t diff = cur->size - size;
        if (diff < best_diff)
        {
          best_diff = diff;
          best_prev = prev;
        }
      }
      prev = cur;
      cur = cur->next;
    }

    if (best_prev != nullptr)
    {
      LargeBlockNode *hit = best_prev->next;
      if (best_prev == tls_large_block_list)
      {
        tls_large_block_list = hit->next;
      }
      else
      {
        best_prev->next = hit->next;
      }
      tls_large_block_num--;
      ibv_mr *mr = hit->mr;
      delete hit;
      return mr;
    }

    // Fallback: allocate + register.
    void *buf = nullptr;
    if (posix_memalign(&buf, 4096, size) != 0)
    {
      LOG(ERROR) << "LargeBlockAlloc: posix_memalign failed";
      return nullptr;
    }
    ibv_mr *mr = ibv_reg_mr(cm_id_->pd, buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (mr == nullptr)
    {
      LOG(ERROR) << "LargeBlockAlloc: ibv_reg_mr failed";
      free(buf);
      return nullptr;
    }
    return mr;
  }

  void UniqueResource::ReturnLargeBlock(ibv_mr *mr)
  {
    if (!mr)
      return;

    if (!tls_large_inited)
    {
      tls_large_inited = true;
      ThreadExitHelper::add_callback([]
                                     { RecycleTLSLargeBlocks(); });
    }

    // Best-fit insert: keep list sorted by size.
    size_t size = mr->length;
    LargeBlockNode *node = new LargeBlockNode{mr->addr, size, mr, 0, nullptr};
    if (tls_large_block_list == nullptr || size < tls_large_block_list->size)
    {
      node->next = tls_large_block_list;
      tls_large_block_list = node;
    }
    else
    {
      LargeBlockNode *cur = tls_large_block_list;
      while (cur->next != nullptr && cur->next->size <= size)
        cur = cur->next;
      node->next = cur->next;
      cur->next = node;
    }
    tls_large_block_num++;
    if (tls_large_block_num > kMaxCachedLargeBlocks)
    {
      LargeBlockNode *last = tls_large_block_list;
      while (last->next != nullptr && last->next->next != nullptr)
        last = last->next;
      LargeBlockNode *to_free = last->next;
      last->next = nullptr;
      CHECK(ibv_dereg_mr(to_free->mr) == 0);
      free(to_free->buf);
      delete to_free;
      tls_large_block_num--;
    }
  }

  UniqueResource::UniqueResource(std::string local_ip, int local_port)
      : FastResource(local_ip, local_port)
  {
    CreateRDMAResource();
    InitBlockPool();
  }

  UniqueResource::~UniqueResource()
  {
    RecycleTLSLargeBlocks();
    CHECK(ibv_destroy_qp(cm_id_->qp) == 0);
    CHECK(ibv_destroy_cq(cm_id_->recv_cq) == 0);
    CHECK(ibv_destroy_cq(cm_id_->send_cq) == 0);
    CHECK(ibv_destroy_comp_channel(cm_id_->recv_cq_channel) == 0);
    CHECK(ibv_destroy_comp_channel(cm_id_->send_cq_channel) == 0);
  }

} // namespace fast
