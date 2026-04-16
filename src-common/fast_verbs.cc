#include "inc/fast_verbs.h"
#include "inc/fast_log.h"

namespace fast
{

  thread_local uint64_t send_counter = 0;


  void SendInlineMessage(ibv_qp *qp,
                         MessageType msg_type,
                         uint64_t msg_addr,
                         uint32_t msg_len)
  {
    ibv_sge send_sg;
    send_sg.addr = msg_addr;
    send_sg.length = msg_len;

    ibv_send_wr send_wr;
    ibv_send_wr *send_bad_wr = nullptr;
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id = 0; // 0
    send_wr.num_sge = 1;
    send_wr.sg_list = &send_sg;
    send_wr.imm_data = htonl(msg_type);
    send_wr.opcode = IBV_WR_SEND_WITH_IMM;
#ifdef TEST_SELECTIVE_SIGNALING
    // Use selective signaling to reduce CQE overhead.
    if (send_counter % 16 == 0)
    {
      send_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED | IBV_SEND_INLINE;
    }
    else
    {
      send_wr.send_flags = IBV_SEND_INLINE | IBV_SEND_SOLICITED; // no CQE
    }
    send_counter++;
#else
    send_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED | IBV_SEND_INLINE;
#endif
    CHECK(ibv_post_send(qp, &send_wr, &send_bad_wr) == 0);
  }

  void SendSmallMessage(ibv_qp *qp,
                        MessageType msg_type,
                        uint64_t msg_addr,
                        uint32_t msg_len,
                        uint32_t lkey)
  {
    ibv_sge send_sg;
    send_sg.addr = msg_addr;
    send_sg.length = msg_len;
    send_sg.lkey = lkey;

    ibv_send_wr send_wr;
    ibv_send_wr *send_bad_wr = nullptr;
    memset(&send_wr, 0, sizeof(send_wr));
    // Set wr_id to block address.
    send_wr.wr_id = send_counter;
    send_wr.num_sge = 1;
    send_wr.sg_list = &send_sg;
    send_wr.imm_data = htonl(msg_type);
    send_wr.opcode = IBV_WR_SEND_WITH_IMM;
#ifdef TEST_SELECTIVE_SIGNALING
    // Use selective signaling to reduce CQE overhead.
    if (send_counter % 16 == 0)
    {
      send_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED | IBV_SEND_INLINE;
    }
    else
    {
      send_wr.send_flags = IBV_SEND_INLINE | IBV_SEND_SOLICITED; // no CQE
    }
    send_counter++;
#else
    send_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED | IBV_SEND_INLINE;
#endif
    CHECK(ibv_post_send(qp, &send_wr, &send_bad_wr) == 0);
  }

  void WriteInlineMessage(ibv_qp *qp,
                          uint64_t msg_addr,
                          uint32_t msg_len,
                          uint32_t remote_key,
                          uint64_t remote_addr)
  {
    ibv_sge write_sg;
    write_sg.addr = msg_addr;
    write_sg.length = msg_len;

    ibv_send_wr write_wr;
    ibv_send_wr *write_bad_wr = nullptr;
    memset(&write_wr, 0, sizeof(write_wr));
    write_wr.wr_id = 0; // 0
    write_wr.num_sge = 1;
    write_wr.sg_list = &write_sg;
    write_wr.opcode = IBV_WR_RDMA_WRITE;
#ifdef TEST_SELECTIVE_SIGNALING
    // Use selective signaling to reduce CQE overhead.
    if (send_counter % 16 == 0)
    {
      write_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    }
    else
    {
      write_wr.send_flags = IBV_SEND_INLINE;
    }
    send_counter++;
#else
    write_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
#endif
    write_wr.wr.rdma.rkey = remote_key;
    write_wr.wr.rdma.remote_addr = remote_addr;
    CHECK(ibv_post_send(qp, &write_wr, &write_bad_wr) == 0);
  }

  void WriteLargeMessage(ibv_qp *qp,
                         ibv_mr *msg_mr,
                         uint32_t msg_len,
                         uint32_t remote_key,
                         uint64_t remote_addr)
  {
    ibv_sge write_sg;
    write_sg.addr = reinterpret_cast<uint64_t>(msg_mr->addr);
    write_sg.length = msg_len; // important!
    write_sg.lkey = msg_mr->lkey;

    ibv_send_wr write_wr;
    ibv_send_wr *write_bad_wr = nullptr;
    memset(&write_wr, 0, sizeof(write_wr));
    // Set wr_id to MR address.
    write_wr.wr_id = send_counter;
    write_wr.num_sge = 1;
    write_wr.sg_list = &write_sg;
    write_wr.opcode = IBV_WR_RDMA_WRITE;
#ifdef TEST_SELECTIVE_SIGNALING
    // Use selective signaling to reduce CQE overhead.
    if (send_counter % 16 == 0)
    {
      write_wr.send_flags = IBV_SEND_SIGNALED;
    }
    else
    {
      write_wr.send_flags = 0; // no CQE
    }
    send_counter++;
#else
    write_wr.send_flags = IBV_SEND_SIGNALED;
#endif
    write_wr.wr.rdma.rkey = remote_key;
    write_wr.wr.rdma.remote_addr = remote_addr;
    CHECK(ibv_post_send(qp, &write_wr, &write_bad_wr) == 0);
  }

  int SendScatterGatherMessage(ibv_qp *qp,
                               MessageType msg_type,
                               const IOBuf &msg,
                               uint32_t total_length,
                               uint32_t lkey)
  {
    const size_t nref = msg.ref_num();
    if (nref == 0)
      return 0;
    if (nref > (size_t)MAX_SGE)
    {
      LOG(WARNING) << "IOBuf has " << nref << " blocks, exceeds max_sge=" << MAX_SGE;
      return -1;
    }

    ibv_sge sges[MAX_SGE];
    size_t sge_idx = 0;
    for (size_t i = 0; i < nref; ++i)
    {
      const IOBuf::BlockRef &r = msg.ref_at(i);
      if (r.length == 0)
        continue;
      sges[sge_idx].addr = reinterpret_cast<uint64_t>(r.block->data + r.offset);
      sges[sge_idx].length = r.length;
      sges[sge_idx].lkey = lkey;
      ++sge_idx;
    }
    if (sge_idx == 0)
      return 0;

    ibv_send_wr wr;
    ibv_send_wr *bad_wr = nullptr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = send_counter++;
    wr.num_sge = sge_idx;
    wr.sg_list = sges;
    wr.imm_data = htonl(msg_type);
    wr.opcode = IBV_WR_SEND_WITH_IMM;
#ifdef TEST_SELECTIVE_SIGNALING
    if (send_counter % 16 == 0)
    {
      wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED;
    }
    else
    {
      wr.send_flags = IBV_SEND_SOLICITED; // no CQE
    }
#else
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED;
#endif
    CHECK(ibv_post_send(qp, &wr, &bad_wr) == 0);
    return sge_idx;
  }

  int WriteScatterGatherMessage(ibv_qp *qp,
                                const IOBuf &msg,
                                uint32_t total_length,
                                uint32_t lkey,
                                uint32_t remote_key,
                                uint64_t remote_addr)
  {
    const size_t nref = msg.ref_num();
    if (nref == 0)
      return 0;
    if (nref > (size_t)MAX_SGE)
    {
      LOG(WARNING) << "IOBuf has " << nref << " blocks, exceeds max_sge=" << MAX_SGE;
      return -1;
    }

    ibv_sge sges[MAX_SGE];
    size_t sge_idx = 0;
    uint32_t offset = 0;
    for (size_t i = 0; i < nref; ++i)
    {
      const IOBuf::BlockRef &r = msg.ref_at(i);
      if (r.length == 0)
        continue;
      sges[sge_idx].addr = reinterpret_cast<uint64_t>(r.block->data + r.offset);
      sges[sge_idx].length = r.length;
      sges[sge_idx].lkey = lkey;
      ++sge_idx;
    }
    if (sge_idx == 0)
      return 0;

    ibv_send_wr wr;
    ibv_send_wr *bad_wr = nullptr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = send_counter++;
    wr.num_sge = sge_idx;
    wr.sg_list = sges;
    wr.opcode = IBV_WR_RDMA_WRITE;
#ifdef TEST_SELECTIVE_SIGNALING
    if (send_counter % 16 == 0)
    {
      wr.send_flags = IBV_SEND_SIGNALED;
    }
    else
    {
      wr.send_flags = 0; // no CQE
    }
#else
    wr.send_flags = IBV_SEND_SIGNALED;
#endif
    wr.wr.rdma.rkey = remote_key;
    wr.wr.rdma.remote_addr = remote_addr;
    CHECK(ibv_post_send(qp, &wr, &bad_wr) == 0);
    return sge_idx;
  }

} // namespace fast