#include <boost/circular_buffer.hpp>
#include "inc/fast_channel.h"
#include "inc/fast_define.h"
#include "inc/fast_log.h"
#include "inc/fast_verbs.h"
#include "build/fast_impl.pb.h"

namespace fast
{

  extern thread_local uint64_t send_counter;
  thread_local boost::circular_buffer<AddressInfo> ibvsend_client_addrs(32);

  FastChannel::FastChannel(UniqueResource *unique_rsc, std::string dest_ip, int dest_port)
      : unique_rsc_(unique_rsc), rpc_id_(1)
  {
    // Resolve remote address.
    sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(dest_ip.c_str());
    dest_addr.sin_port = htons(dest_port);

    auto cm_id = unique_rsc_->GetConnMgrID();

    CHECK(rdma_resolve_addr(cm_id, nullptr, reinterpret_cast<sockaddr *>(&dest_addr), timeout_in_ms) == 0);
    rdma_cm_event *cm_ent = nullptr;
    CHECK(rdma_get_cm_event(cm_id->channel, &cm_ent) == 0);
    CHECK(cm_ent->event == RDMA_CM_EVENT_ADDR_RESOLVED);
    CHECK(rdma_ack_cm_event(cm_ent) == 0);

    // Resolve an RDMA route to the destination address.
    CHECK(rdma_resolve_route(cm_id, timeout_in_ms) == 0);
    CHECK(rdma_get_cm_event(cm_id->channel, &cm_ent) == 0);
    CHECK(cm_ent->event == RDMA_CM_EVENT_ROUTE_RESOLVED);
    CHECK(rdma_ack_cm_event(cm_ent) == 0);

    // Connect to remote address.
    rdma_conn_param cm_params;
    memset(&cm_params, 0, sizeof(cm_params));
    cm_params.rnr_retry_count = 7; // infinite retry
    CHECK(rdma_connect(cm_id, &cm_params) == 0);
    CHECK(rdma_get_cm_event(cm_id->channel, &cm_ent) == 0);
    CHECK(cm_ent->event == RDMA_CM_EVENT_ESTABLISHED);
    CHECK(rdma_ack_cm_event(cm_ent) == 0);
  }

  FastChannel::~FastChannel()
  {
    auto cm_id = unique_rsc_->GetConnMgrID();

    CHECK(rdma_disconnect(cm_id) == 0);
    rdma_cm_event *cm_ent = nullptr;
    CHECK(rdma_get_cm_event(cm_id->channel, &cm_ent) == 0);
    CHECK(cm_ent->event == RDMA_CM_EVENT_DISCONNECTED);
    CHECK(rdma_ack_cm_event(cm_ent) == 0);
  }

  void FastChannel::CallMethod(const google::protobuf::MethodDescriptor *method,
                               google::protobuf::RpcController *controller,
                               const google::protobuf::Message *request,
                               google::protobuf::Message *response,
                               google::protobuf::Closure *done)
  {
    uint32_t rpc_id = rpc_id_++;

    // Step 1: Calculate sizes.
    MetaDataOfRequest meta;
    meta.set_rpc_id(rpc_id);
    meta.set_service_name(method->service()->name());
    meta.set_method_name(method->name());
    uint32_t meta_len = meta.ByteSizeLong();
    uint32_t payload_len = request->ByteSizeLong();
    uint32_t attachment_len = request_attachment_.length();
    meta.set_attachment_size(attachment_len);

    uint32_t total_length = 2 * fixed32_bytes + meta_len + payload_len + attachment_len;

    LengthOfMetaData part2;
    part2.set_metadata_len(meta_len);
    CHECK(part2.ByteSizeLong() == fixed32_bytes);

    TotalLengthOfMsg part1;
    part1.set_total_len(total_length);
    CHECK(part1.ByteSizeLong() == fixed32_bytes);

    /// NOTE: Post one recv WR for receiving response.
    uint64_t block_addr = 0;
    unique_rsc_->ObtainOneBlock(block_addr);
    unique_rsc_->PostOneRecvRequest(block_addr);

    ibv_qp *local_qp = unique_rsc_->GetConnMgrID()->qp;
    uint32_t local_key = unique_rsc_->GetLocalKey();
    uint32_t remote_key = unique_rsc_->GetRemoteKey();

    if (total_length <= max_inline_data)
    {
      // Inline path: build frame into stack buffer, then send inline.
      char msg_buf[max_inline_data];

      // Build frame: [|total_len|metadata_len|][meta][payload][attachment]
      char *dst = msg_buf;
      memcpy(dst, &total_length, fixed32_bytes);
      dst += fixed32_bytes;
      memcpy(dst, &meta_len, fixed32_bytes);
      dst += fixed32_bytes;

      // Serialize meta via ZeroCopyOutputStream into a temp buffer
      char meta_buf[256];
      CHECK(meta_len <= sizeof(meta_buf));
      {
        google::protobuf::io::ArrayOutputStream arr_out(meta_buf, meta_len);
        google::protobuf::io::CodedOutputStream coded(&arr_out);
        meta.SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
      }
      memcpy(dst, meta_buf, meta_len);
      dst += meta_len;

      // Serialize payload
      uint32_t remaining = max_inline_data - (dst - msg_buf);
      CHECK(payload_len <= remaining);
      google::protobuf::io::ArrayOutputStream arr_out(dst, remaining);
      CHECK(request->SerializeToZeroCopyStream(&arr_out));
      dst += payload_len;

      // Append attachment
      if (attachment_len > 0)
      {
        for (size_t i = 0; i < request_attachment_.ref_num(); ++i)
        {
          const auto &r = request_attachment_.ref_at(i);
          memcpy(dst, r.block->data + r.offset, r.length);
          dst += r.length;
        }
      }

      uint64_t msg_addr = reinterpret_cast<uint64_t>(msg_buf);
      SendInlineMessage(local_qp, FAST_SmallMessage, msg_addr, total_length);
    }
    else if (total_length <= msg_threshold)
    {
      // Small message path: build frame into IOBuf backed by block_pool blocks,
      // serialize header+body via IOBufAsZeroCopyOutputStream (zero-copy),
      // then post RDMA SEND with one SGE per block.
      IOBuf frame;

      // Append fixed headers.
      frame.append(&total_length, fixed32_bytes);
      frame.append(&meta_len, fixed32_bytes);

      // Serialize meta into IOBuf via ZeroCopyOutputStream.
      {
        IOBufAsZeroCopyOutputStream zcos(&frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        meta.SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
      }

      // Serialize payload into IOBuf via ZeroCopyOutputStream.
      {
        IOBufAsZeroCopyOutputStream zcos(&frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        request->SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
      }

      // Append attachment.
      if (attachment_len > 0)
      {
        frame.append(request_attachment_);
      }

      // Post RDMA SEND: one SGE per IOBuf block.
      ibv_sge sges[MAX_SGE];
      int sge_count = 0;
      for (size_t i = 0; i < frame.ref_num(); ++i)
      {
        const auto &r = frame.ref_at(i);
        sges[sge_count].addr = reinterpret_cast<uint64_t>(r.block->data) + r.offset;
        sges[sge_count].length = r.length;
        sges[sge_count].lkey = local_key;
        ++sge_count;
        ibvsend_client_addrs.push_back(AddressInfo(BLOCK_ADDRESS, reinterpret_cast<uint64_t>(r.block->data) + r.offset, send_counter));
      }

      ibv_send_wr wr;
      ibv_send_wr *bad_wr = nullptr;
      memset(&wr, 0, sizeof(wr));
      wr.wr_id = send_counter++;
      wr.num_sge = sge_count;
      wr.sg_list = sges;
      wr.imm_data = htonl(FAST_SmallMessage);
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
      CHECK(ibv_post_send(local_qp, &wr, &bad_wr) == 0);
    }
    else
    {
      // Large message path.
      // Build frame into IOBuf via IOBufAsZeroCopyOutputStream, then:
      // - Try scatter-gather if frame fits in <= MAX_SGE blocks
      // - Fallback: copy into large registered MR → RDMA write

      // Obtain one block for receiving authority message.
      uint64_t auth_addr = 0;
      unique_rsc_->ObtainOneBlock(auth_addr);
      char *auth_buf = reinterpret_cast<char *>(auth_addr);
      memset(auth_buf + fixed_auth_bytes, '0', 1);

      // Step-1: Create and send the notify message (send op).
      NotifyMessage notify_msg;
      notify_msg.set_total_len(total_length);
      notify_msg.set_remote_key(remote_key);
      notify_msg.set_remote_addr(auth_addr);
      CHECK(notify_msg.ByteSizeLong() == fixed_noti_bytes);
      char noti_buf[max_inline_data];
      uint64_t noti_addr = reinterpret_cast<uint64_t>(noti_buf);
      CHECK(notify_msg.SerializeToArray(noti_buf, fixed_noti_bytes));
      SendInlineMessage(local_qp, FAST_NotifyMessage, noti_addr, fixed_noti_bytes);

      // Step-2: Build entire frame in IOBuf via ZeroCopyOutputStream.
      IOBuf frame;
      frame.append(&total_length, fixed32_bytes);
      frame.append(&meta_len, fixed32_bytes);

      // Serialize meta into IOBuf.
      {
        IOBufAsZeroCopyOutputStream zcos(&frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        meta.SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
      }

      // Serialize payload into IOBuf.
      {
        IOBufAsZeroCopyOutputStream zcos(&frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        request->SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
      }

      // Append attachment.
      if (attachment_len > 0)
      {
        frame.append(request_attachment_);
      }

      // Step-3: Send the frame via scatter-gather or fallback.
      if (frame.ref_num() <= (size_t)MAX_SGE)
      {
        // Scatter-gather: copy each IOBuf TLS block → RDMA-registered block → multi-SGE SEND.
        ibv_sge sges[MAX_SGE];
        int sge_count = 0;
        for (size_t i = 0; i < frame.ref_num(); ++i)
        {
          const auto &r = frame.ref_at(i);
          uint64_t rdma_block = 0;
          unique_rsc_->ObtainOneBlock(rdma_block);
          memcpy(reinterpret_cast<char *>(rdma_block), r.block->data + r.offset, r.length);
          sges[sge_count].addr = rdma_block;
          sges[sge_count].length = r.length;
          sges[sge_count].lkey = local_key;
          ++sge_count;
          ibvsend_client_addrs.push_back(AddressInfo(BLOCK_ADDRESS, rdma_block, send_counter));
        }

        ibv_send_wr wr;
        ibv_send_wr *bad_wr = nullptr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = send_counter++;
        wr.num_sge = sge_count;
        wr.sg_list = sges;
        wr.imm_data = htonl(FAST_SmallMessage);
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
        CHECK(ibv_post_send(local_qp, &wr, &bad_wr) == 0);

        // Wait for authority, then return auth block.
        volatile char *flag = auth_buf + fixed_auth_bytes;
        while (*flag != '1') { }
        AuthorityMessage authority_msg;
        CHECK(authority_msg.ParseFromArray(auth_buf, fixed_auth_bytes));
        unique_rsc_->ReturnOneBlock(auth_addr);
      }
      else
      {
        // Fallback: copy frame into large MR, RDMA write.
        ibv_mr *large_send_mr = unique_rsc_->GetOneMRFromCache(total_length + 1);
        if (large_send_mr == nullptr)
        {
          large_send_mr = unique_rsc_->AllocAndRegisterMR(total_length + 1);
        }
        char *msg_buf = reinterpret_cast<char *>(large_send_mr->addr);

        // Copy IOBuf frame into large MR.
        char *dst = msg_buf;
        for (size_t i = 0; i < frame.ref_num(); ++i)
        {
          const auto &r = frame.ref_at(i);
          memcpy(dst, r.block->data + r.offset, r.length);
          dst += r.length;
        }
        memset(msg_buf + total_length, '1', 1);  // authority flag

        // Wait for authority, then RDMA write.
        volatile char *flag = auth_buf + fixed_auth_bytes;
        while (*flag != '1') { }
        AuthorityMessage authority_msg;
        CHECK(authority_msg.ParseFromArray(auth_buf, fixed_auth_bytes));

        WriteLargeMessage(local_qp,
                          large_send_mr,
                          total_length + 1,
                          authority_msg.remote_key(),
                          authority_msg.remote_addr());
        ibvsend_client_addrs.push_back(AddressInfo(MR_ADDRESS, reinterpret_cast<uint64_t>(large_send_mr), send_counter));
      }
      // frame goes out of scope here: TLS blocks returned to TLS free list.
    }

    // NOTE: Clear attachment after use.
    request_attachment_.clear();

    /// NOTE: Try to poll send CQEs.
    this->TryToPollSendWC();

    ibv_cq *recv_cq = unique_rsc_->GetConnMgrID()->recv_cq;
    ibv_wc recv_wc;
    memset(&recv_wc, 0, sizeof(recv_wc));
    while (true)
    {
      int num = ibv_poll_cq(recv_cq, 1, &recv_wc);
      CHECK(num != -1);
      if (num == 1)
        break; // Just get one WC.
    }

    CHECK(recv_wc.status == IBV_WC_SUCCESS);
    uint32_t imm_data = ntohl(recv_wc.imm_data);
    uint64_t recv_addr = recv_wc.wr_id;

    if (imm_data == FAST_SmallMessage)
    {
      char *msg_buf = reinterpret_cast<char *>(recv_addr);
      ParseAndProcessResponse(msg_buf, true, response);
    }
    else
    {
      CHECK(imm_data == FAST_NotifyMessage);
      auto large_recv_mr = ProcessNotifyMessage(recv_addr);
      ParseAndProcessResponse(large_recv_mr, false, response);
    }
  }

  void FastChannel::TryToPollSendWC()
  {
    ibv_cq *send_cq = unique_rsc_->GetConnMgrID()->send_cq;
    ibv_wc send_wc;
    memset(&send_wc, 0, sizeof(send_wc));
    while (true)
    {
      int num = ibv_poll_cq(send_cq, 1, &send_wc);
      CHECK(num != -1);
      if (num == 0)
        break;
      CHECK(send_wc.status == IBV_WC_SUCCESS);
      ProcessSendWorkCompletion(send_wc);
      memset(&send_wc, 0, sizeof(send_wc));
    }
  }

  void FastChannel::ProcessSendWorkCompletion(ibv_wc &send_wc)
  {
    if (send_wc.wr_id == 0)
      return;
    while (ibvsend_client_addrs.size() > 0 && ibvsend_client_addrs.front().send_counter <= send_wc.wr_id)
    {
      AddressInfo info = ibvsend_client_addrs.front();
      if (info.type == BLOCK_ADDRESS)
      {
        unique_rsc_->ReturnOneBlock(info.addr);
      }
      else
      {
        unique_rsc_->PutOneMRIntoCache(reinterpret_cast<ibv_mr *>(info.addr));
      }
      ibvsend_client_addrs.pop_front();
    }
  }

  ibv_mr *FastChannel::ProcessNotifyMessage(uint64_t block_addr)
  {
    ibv_qp *local_qp = unique_rsc_->GetConnMgrID()->qp;

    // Step-1: Get the notify message.
    char *block_buf = reinterpret_cast<char *>(block_addr);
    NotifyMessage notify_msg;
    CHECK(notify_msg.ParseFromArray(block_buf, fixed_noti_bytes));
    // Return the block which stores notify message.
    unique_rsc_->ReturnOneBlock(block_addr);
    uint32_t total_length = notify_msg.total_len();

    // Step-2: Prepare a large MR.
    ibv_mr *large_recv_mr = unique_rsc_->GetOneMRFromCache(total_length + 1);
    if (large_recv_mr == nullptr)
    {
      large_recv_mr = unique_rsc_->AllocAndRegisterMR(total_length + 1);
    }
    char *msg_buf = reinterpret_cast<char *>(large_recv_mr->addr);
    // [Receiver]: set the flag byte to '0'.
    memset(msg_buf + total_length, '0', 1);

    // Step-3: Create and send the authority message (write op).
    AuthorityMessage authority_msg;
    authority_msg.set_remote_key(large_recv_mr->rkey);
    authority_msg.set_remote_addr(reinterpret_cast<uint64_t>(large_recv_mr->addr));
    CHECK(authority_msg.ByteSizeLong() == fixed_auth_bytes);
    char auth_buf[max_inline_data];
    uint64_t auth_addr = reinterpret_cast<uint64_t>(auth_buf);
    CHECK(authority_msg.SerializeToArray(auth_buf, fixed_auth_bytes));
    // [Sender]: set the flag byte to '1'.
    memset(auth_buf + fixed_auth_bytes, '1', 1);
    WriteInlineMessage(local_qp,
                       auth_addr,
                       fixed_auth_bytes + 1,
                       notify_msg.remote_key(),
                       notify_msg.remote_addr());

    /// NOTE: Try to poll send CQEs.
    this->TryToPollSendWC();

    // Step-3: Get the large message (busy check).
    volatile char *flag = msg_buf + total_length;
    while (*flag != '1')
    {
    }

    return large_recv_mr;
  }

  void FastChannel::ParseAndProcessResponse(void *addr,
                                            bool small_msg,
                                            google::protobuf::Message *response)
  {
    char *msg_buf = nullptr;
    if (small_msg)
    {
      msg_buf = reinterpret_cast<char *>(addr);
    }
    else
    {
      auto large_mr = reinterpret_cast<ibv_mr *>(addr);
      msg_buf = reinterpret_cast<char *>(large_mr->addr);
    }

    ResponseHead part1;
    part1.ParseFromArray(msg_buf, fixed_rep_head_bytes);
    uint32_t rpc_id = part1.rpc_id();
    uint32_t total_length = part1.total_len();
    uint32_t attachment_size = part1.attachment_size();

    uint32_t payload_length = total_length - fixed_rep_head_bytes;
    if (attachment_size > 0)
    {
      payload_length -= attachment_size;
    }

    response_attachment_.clear();
    if (attachment_size > 0)
    {
      const char *attachment_start = msg_buf + fixed_rep_head_bytes + payload_length;
      response_attachment_.append(attachment_start, attachment_size);
    }

    // Parse payload via ZeroCopyInputStream.
    IOBuf payload_iobuf;
    payload_iobuf.append(msg_buf + fixed_rep_head_bytes, payload_length);
    IOBufAsZeroCopyInputStream zcis(payload_iobuf);
    CHECK(response->ParseFromZeroCopyStream(&zcis));

    /// NOTE: Return the occupied resources.
    if (small_msg)
    {
      uint64_t block_addr = reinterpret_cast<uint64_t>(addr);
      unique_rsc_->ReturnOneBlock(block_addr);
    }
    else
    {
      unique_rsc_->PutOneMRIntoCache(reinterpret_cast<ibv_mr *>(addr));
    }
  }

} // namespace fast
