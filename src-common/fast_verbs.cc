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

  void PostScatterGatherSend(ibv_qp *qp,
                              ibv_sge *sges,
                              int sge_count,
                              MessageType msg_type)
  {
    ibv_send_wr wr;
    ibv_send_wr *bad_wr = nullptr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = send_counter++;
    wr.num_sge = sge_count;
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
      wr.send_flags = IBV_SEND_SOLICITED;
    }
#else
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED;
#endif
    CHECK(ibv_post_send(qp, &wr, &bad_wr) == 0);
  }

  void PostScatterGatherWrite(ibv_qp *qp,
                               ibv_sge *sges,
                               int sge_count,
                               uint32_t remote_key,
                               uint64_t remote_addr)
  {
    ibv_send_wr wr;
    ibv_send_wr *bad_wr = nullptr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = send_counter++;
    wr.num_sge = sge_count;
    wr.sg_list = sges;
    wr.opcode = IBV_WR_RDMA_WRITE;
#ifdef TEST_SELECTIVE_SIGNALING
    if (send_counter % 16 == 0)
    {
      wr.send_flags = IBV_SEND_SIGNALED;
    }
    else
    {
      wr.send_flags = 0;
    }
#else
    wr.send_flags = IBV_SEND_SIGNALED;
#endif
    wr.wr.rdma.rkey = remote_key;
    wr.wr.rdma.remote_addr = remote_addr;
    CHECK(ibv_post_send(qp, &wr, &bad_wr) == 0);
  }

  void PostScatterGatherWriteWithOffsets(ibv_qp *qp,
                                         ibv_sge *sges,
                                         uint64_t *offsets,
                                         int sge_count,
                                         uint32_t remote_key,
                                         uint64_t remote_addr)
  {
    ibv_send_wr wr;
    ibv_send_wr *bad_wr = nullptr;
    for (int i = 0; i < sge_count; ++i)
    {
      memset(&wr, 0, sizeof(wr));
      wr.wr_id = send_counter++;
      wr.num_sge = 1;
      wr.sg_list = &sges[i];
      wr.opcode = IBV_WR_RDMA_WRITE;
#ifdef TEST_SELECTIVE_SIGNALING
      if (wr.wr_id % 16 == 0)
      {
        wr.send_flags = IBV_SEND_SIGNALED;
      }
      else
      {
        wr.send_flags = 0;
      }
#else
      wr.send_flags = IBV_SEND_SIGNALED;
#endif
      wr.wr.rdma.rkey = remote_key;
      wr.wr.rdma.remote_addr = remote_addr + offsets[i];
      CHECK(ibv_post_send(qp, &wr, &bad_wr) == 0);
    }
  }

} // namespace fast