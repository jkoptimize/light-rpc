#include "inc/fast_shared.h"
#include "inc/fast_log.h"

static const int min_cqe_num = 512;
static const uint32_t max_srq_wr = 512;
static const uint32_t max_send_wr = 32;
static const uint32_t max_num_sge = 1;
static const int num_cpus = std::thread::hardware_concurrency();

namespace fast
{

  const int SharedResource::default_num_work_threads = num_cpus;

  SharedResource::SharedResource(std::string local_ip,
                                 int local_port,
                                 int num_work_th)
      : FastResource(local_ip, local_port),
        num_work_threads_(num_work_th),
        schedule_idx_(0)
  {
    CreateRDMAResource();
    InitBlockPool();

    for (int i = 0; i < num_work_threads_; ++i)
    {
      io_ctx_vec_.emplace_back(std::make_unique<boost::asio::io_context>());
      auto &io_ctx = *io_ctx_vec_[i];
      threads_vec_.emplace_back([&io_ctx]
                                {
      boost::asio::io_context::work work(io_ctx);
      io_ctx.run(); });
      thid_idx_map_.emplace(threads_vec_[i].get_id(), i);
    }

    // Pre-post recv WRs using blocks from block_pool.
    for (int i = 0; i < max_srq_wr; i++)
    {
      uint64_t block_addr = 0;
      PostOneRecvRequest(block_addr);
    }
  }

  SharedResource::~SharedResource()
  {
    for (int i = 0; i < num_work_threads_; ++i)
    {
      io_ctx_vec_[i]->stop();
      threads_vec_[i].join();
    }
    CHECK(ibv_destroy_srq(cm_id_->srq) == 0);
    CHECK(ibv_destroy_cq(cm_id_->recv_cq) == 0);
    CHECK(ibv_destroy_comp_channel(cm_id_->recv_cq_channel) == 0);
  }

  void SharedResource::CreateRDMAResource()
  {
    ibv_srq_init_attr srq_init_attr;
    memset(&srq_init_attr, 0, sizeof(srq_init_attr));
    srq_init_attr.attr.max_sge = max_num_sge;
    srq_init_attr.attr.max_wr = max_srq_wr;
    cm_id_->srq = ibv_create_srq(cm_id_->pd, &srq_init_attr);
    CHECK(cm_id_->srq != nullptr);

    cm_id_->recv_cq_channel = ibv_create_comp_channel(cm_id_->verbs);
    CHECK(cm_id_->recv_cq_channel != nullptr);
    cm_id_->recv_cq = ibv_create_cq(
        cm_id_->verbs, min_cqe_num, nullptr, cm_id_->recv_cq_channel, 0);
    CHECK(cm_id_->recv_cq != nullptr);
  }

  void SharedResource::InitBlockPool()
  {
    // Set the global PD and init the block pool (idempotent: skip if already inited).
    ::fast::SetGlobalPD(cm_id_->pd);
    ::fast::InitBlockPool();
  }

  void SharedResource::PostOneRecvRequest(uint64_t block_addr)
  {
    ibv_sge recv_sg;
    recv_sg.addr = block_addr;
    recv_sg.length = msg_threshold;
    recv_sg.lkey = ::fast::GetRegionId(reinterpret_cast<void *>(block_addr));

    ibv_recv_wr recv_wr;
    ibv_recv_wr *recv_bad_wr = nullptr;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = block_addr;
    recv_wr.num_sge = 1;
    recv_wr.sg_list = &recv_sg;
    CHECK(ibv_post_srq_recv(cm_id_->srq, &recv_wr, &recv_bad_wr) == 0);
  }

  void SharedResource::CreateNewQueuePair(rdma_cm_id *conn_id)
  {
    conn_id->send_cq_channel = ibv_create_comp_channel(conn_id->verbs);
    CHECK(conn_id->send_cq_channel != nullptr);
    conn_id->send_cq = ibv_create_cq(
        conn_id->verbs, min_cqe_num, nullptr, conn_id->send_cq_channel, 0);
    CHECK(conn_id->send_cq != nullptr);

    ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.sq_sig_all = 0;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.srq = cm_id_->srq;
    qp_attr.send_cq = conn_id->send_cq;
    qp_attr.recv_cq = cm_id_->recv_cq;
    qp_attr.cap.max_send_wr = max_send_wr;
    qp_attr.cap.max_send_sge = max_num_sge;
    qp_attr.cap.max_inline_data = max_inline_data;
    CHECK(rdma_create_qp(conn_id, conn_id->pd, &qp_attr) == 0);
  }

} // namespace fast
