#include "inc/fast_server.h"
#include "inc/fast_define.h"
#include "inc/fast_log.h"
#include "inc/fast_verbs.h"
#include "inc/fast_block_pool.h"
#include "build/fast_impl.pb.h"

namespace fast
{

  const int FastServer::default_num_poll_th = std::max(1U, std::thread::hardware_concurrency() / 8U);

  extern thread_local uint64_t send_counter;
  thread_local boost::circular_buffer<AddressInfo> ibvsend_server_addrs(32);

  FastServer::FastServer(SharedResource *shared_rsc, int num_poll_th)
      : shared_rsc_(shared_rsc),
        num_pollers_(num_poll_th),
        stop_flag_(false)
  {
    for (int i = 0; i < num_pollers_; ++i)
    {
      this->poller_pool_.emplace_back([this]
                                      { this->BusyPollRecvWC(); });
    }
    CHECK(ibv_req_notify_cq(shared_rsc_->GetConnMgrID()->recv_cq, 1) == 0);
    this->conn_id_map_ = std::make_unique<SafeHashMap<rdma_cm_id *>>();
    CHECK(rdma_listen(shared_rsc_->GetConnMgrID(), listen_backlog) == 0);
    LOG_INFO("Start listening on port %d", shared_rsc_->GetLocalPort());
  }

  FastServer::~FastServer()
  {
    for (auto &kv : service_map_)
    {
      if (kv.second.ownership == SERVER_OWNS_SERVICE)
      {
        delete kv.second.service;
      }
    }
    stop_flag_ = true;
    for (auto &poll_th : poller_pool_)
    {
      poll_th.join();
    }
  }

  void FastServer::AddService(ServiceOwnership ownership, google::protobuf::Service *service)
  {
    auto str = service->GetDescriptor()->name();
    std::string name(str.data(), str.length());
    CHECK(service_map_.find(name) == service_map_.end());
    ServiceInfo service_info;
    service_info.ownership = ownership;
    service_info.service = service;
    service_map_.emplace(name, service_info);
  }

  void FastServer::BuildAndStart()
  {
    rdma_cm_event_type ent_type;
    rdma_cm_id *conn_id = nullptr;
    rdma_cm_event *cm_ent = nullptr;
    auto listen_id = shared_rsc_->GetConnMgrID();
    while (true)
    {
      CHECK(rdma_get_cm_event(listen_id->channel, &cm_ent) == 0);
      ent_type = cm_ent->event;
      conn_id = cm_ent->id;
      if (ent_type == RDMA_CM_EVENT_CONNECT_REQUEST)
      {
        CHECK(rdma_ack_cm_event(cm_ent) == 0);
        CHECK(conn_id->verbs == listen_id->verbs);
        CHECK(conn_id->pd == listen_id->pd);
        ProcessConnectRequest(conn_id);
      }
      else if (ent_type == RDMA_CM_EVENT_ESTABLISHED)
      {
        CHECK(rdma_ack_cm_event(cm_ent) == 0);
      }
      else if (ent_type == RDMA_CM_EVENT_DISCONNECTED)
      {
        CHECK(rdma_ack_cm_event(cm_ent) == 0);
        ProcessDisconnect(conn_id);
      }
      else
      {
        LOG_ERR("Other event value: %d.", ent_type);
      }
    }
  }

  void FastServer::ProcessConnectRequest(rdma_cm_id *conn_id)
  {
    // Create and record queue pair.
    shared_rsc_->CreateNewQueuePair(conn_id);
    conn_id_map_->SafeInsert(conn_id->qp->qp_num, conn_id);

    // Accept the connection request.
    rdma_conn_param cm_params;
    memset(&cm_params, 0, sizeof(cm_params));
    cm_params.rnr_retry_count = 7; // infinite retry
    CHECK(rdma_accept(conn_id, &cm_params) == 0);
  }

  void FastServer::ProcessDisconnect(rdma_cm_id *conn_id)
  {
    conn_id_map_->SafeErase(conn_id->qp->qp_num);
    // https://blog.lucode.net/RDMA/rdma-cm-introduction-and-create-qp-with-external-cq.html
    rdma_destroy_qp(conn_id);
    CHECK(rdma_destroy_id(conn_id) == 0);
  }

  void FastServer::IBVEventNotifyWait(uint64_t &poll_times)
  {
    auto recv_cq = shared_rsc_->GetConnMgrID()->recv_cq;
    auto recv_cq_channel = shared_rsc_->GetConnMgrID()->recv_cq_channel;
    ibv_cq *ev_cq;
    void *ev_ctx;
    uint64_t num = 0;
    ibv_wc recv_wc;

    int ret = ibv_get_cq_event(recv_cq_channel, &ev_cq, &ev_ctx);
    CHECK(ret != -1);
    CHECK(ev_cq == recv_cq);
    ibv_ack_cq_events(ev_cq, 1);
    CHECK(ibv_req_notify_cq(ev_cq, 1) == 0);

    do
    {
      num = ibv_poll_cq(recv_cq, 1, &recv_wc);
      CHECK(num != -1);
      CHECK(recv_wc.status == IBV_WC_SUCCESS);
      if (num == 0)
        continue;
      ProcessRecvWorkCompletion(recv_wc);
      poll_times = 0;
    } while (num);
  }

  void FastServer::BusyPollRecvWC()
  {
    auto recv_cq = shared_rsc_->GetConnMgrID()->recv_cq;
    ibv_wc recv_wc;
    uint64_t poll_times = 0;
    memset(&recv_wc, 0, sizeof(recv_wc));
    while (true)
    {
      int num = ibv_poll_cq(recv_cq, 1, &recv_wc);
      CHECK(num != -1);
      if (num == 1)
      {
        CHECK(recv_wc.status == IBV_WC_SUCCESS);
        ProcessRecvWorkCompletion(recv_wc);
        poll_times = 0;
      }
      else if (poll_times < cq_poll_min_times)
      {
        poll_times++;
      }
      else if (poll_times < cq_poll_min_times * 2)
      {
        poll_times++;
        std::this_thread::yield();
      }
      else
      {
        IBVEventNotifyWait(poll_times);
      }
    }
  }

  void FastServer::ProcessRecvWorkCompletion(ibv_wc &recv_wc)
  {
    uint32_t imm_data = ntohl(recv_wc.imm_data);
    rdma_cm_id *conn_id = conn_id_map_->SafeGet(recv_wc.qp_num);
    uint64_t recv_addr = recv_wc.wr_id;

    boost::asio::io_context *io_ctx = shared_rsc_->GetIOContext();

    /// NOTE: Post one recv WR for receiving next request.
    uint64_t block_addr;
    shared_rsc_->ObtainOneBlock(block_addr);
    shared_rsc_->PostOneRecvRequest(block_addr);

    if (imm_data == FAST_SmallMessage)
    {
      char *msg_buf = reinterpret_cast<char *>(recv_addr);
      io_ctx->post([this, conn_id, msg_buf]
                   { this->ParseAndProcessRequest(conn_id, msg_buf, true); });
    }
    else
    {
      CHECK(imm_data == FAST_NotifyMessage);
      io_ctx->post([this, conn_id, recv_addr]
                   { this->ProcessNotifyMessage(conn_id, recv_addr); });
    }
  }

  void FastServer::TryToPollSendWC(rdma_cm_id *conn_id)
  {
    ibv_wc send_wc;
    memset(&send_wc, 0, sizeof(send_wc));
    while (true)
    {
      int num = ibv_poll_cq(conn_id->send_cq, 1, &send_wc);
      CHECK(num != -1);
      if (num == 0)
        break;
      CHECK(send_wc.status == IBV_WC_SUCCESS);
      ProcessSendWorkCompletion(send_wc);
      memset(&send_wc, 0, sizeof(send_wc));
    }
  }

  void FastServer::ProcessSendWorkCompletion(ibv_wc &send_wc)
  {
    if (send_wc.wr_id == 0)
      return;
    while (ibvsend_server_addrs.size() > 0 && ibvsend_server_addrs.front().send_counter <= send_wc.wr_id)
    {
      AddressInfo info = ibvsend_server_addrs.front();
      if (info.type == BLOCK_ADDRESS)
      {
        shared_rsc_->ReturnOneBlock(info.addr);
      }
      else
      {
        shared_rsc_->ReturnLargeBlock(info.mr);
      }
      ibvsend_server_addrs.pop_front();
    }
  }

  void FastServer::ProcessNotifyMessage(rdma_cm_id *conn_id, uint64_t block_addr)
  {
    // Step-1: Get the notify message.
    char *block_buf = reinterpret_cast<char *>(block_addr);
    NotifyMessage notify_msg;
    CHECK(notify_msg.ParseFromArray(block_buf, fixed_noti_bytes));
    // Return the block which stores notify message.
    ::fast::BlockDeallocate(block_buf);
    uint32_t total_length = notify_msg.total_len();

    // Step-2: Allocate receive large block.
    ibv_mr *recv_mr = shared_rsc_->LargeBlockAlloc(total_length + 1);
    CHECK(recv_mr != nullptr);
    char *msg_buf = reinterpret_cast<char *>(recv_mr->addr);
    // [Receiver]: set the flag byte to '0'.
    memset(msg_buf + total_length, '0', 1);

    // Step-3: Create and send the authority message (write op).
    AuthorityMessage authority_msg;
    authority_msg.set_remote_key(recv_mr->rkey);
    authority_msg.set_remote_addr(reinterpret_cast<uint64_t>(recv_mr->addr));
    CHECK(authority_msg.ByteSizeLong() == fixed_auth_bytes);
    char auth_buf[max_inline_data];
    uint64_t auth_addr = reinterpret_cast<uint64_t>(auth_buf);
    CHECK(authority_msg.SerializeToArray(auth_buf, fixed_auth_bytes));
    // [Sender]: set the flag byte to '1'.
    memset(auth_buf + fixed_auth_bytes, '1', 1);
    WriteInlineMessage(conn_id->qp,
                       auth_addr,
                       fixed_auth_bytes + 1,
                       notify_msg.remote_key(),
                       notify_msg.remote_addr());

    /// NOTE: Try to poll send CQEs.
    this->TryToPollSendWC(conn_id);

    // Step-4: Get the large message (busy check).
    volatile char *flag = msg_buf + total_length;
    while (*flag != '1')
    {
    }

    this->ParseAndProcessRequest(conn_id, recv_mr, false);
  }

  void FastServer::ParseAndProcessRequest(rdma_cm_id *conn_id, void *addr, bool small_msg)
  {
    char *msg_buf = reinterpret_cast<char *>(addr);

    TotalLengthOfMsg part1;
    part1.ParseFromArray(msg_buf, fixed32_bytes);
    uint32_t total_length = part1.total_len();

    LengthOfMetaData part2;
    part2.ParseFromArray(msg_buf + fixed32_bytes, fixed32_bytes);
    uint32_t metadata_length = part2.metadata_len();

    // Parse metadata via IOBufAsZeroCopyInputStream (zero-copy parse from recv buffer).
    const char *meta_start = msg_buf + 2 * fixed32_bytes;
    IOBuf meta_iobuf;
    meta_iobuf.append(meta_start, metadata_length);
    IOBufAsZeroCopyInputStream zcis(meta_iobuf);
    MetaDataOfRequest part3;
    CHECK(part3.ParseFromZeroCopyStream(&zcis));

    uint32_t rpc_id = part3.rpc_id();
    std::string service_name = part3.service_name();
    std::string method_name = part3.method_name();
    uint32_t attachment_size = part3.attachment_size();

    auto iter = service_map_.find(service_name);
    auto service = iter->second.service;
    auto method = service->GetDescriptor()->FindMethodByName(method_name);

    // Create request and response instance.
    auto request = service->GetRequestPrototype(method).New();
    auto response = service->GetResponsePrototype(method).New();

    uint32_t body_start = 2 * fixed32_bytes + metadata_length;
    uint32_t payload_length = total_length - 2 * fixed32_bytes - metadata_length;
    if (attachment_size > 0)
    {
      payload_length -= attachment_size;
    }

    // Parse payload via IOBufAsZeroCopyInputStream (zero-copy).
    IOBuf payload_iobuf;
    payload_iobuf.append(msg_buf + body_start, payload_length);
    IOBufAsZeroCopyInputStream payload_zcis(payload_iobuf);
    CHECK(request->ParseFromZeroCopyStream(&payload_zcis));

    // Extract attachment into IOBuf.
    IOBuf request_attachment;
    if (attachment_size > 0)
    {
      const char *attachment_start = msg_buf + body_start + payload_length;
      request_attachment.append(attachment_start, attachment_size);
    }

    /// NOTE: Return the occupied resources.
    if (small_msg)
    {
      ::fast::BlockDeallocate(addr);
    }
    else
    {
      auto mr = reinterpret_cast<ibv_mr *>(addr);
      shared_rsc_->ReturnLargeBlock(mr);
    }

    CallBackArgs args;
    args.rpc_id = rpc_id;
    args.conn_id = conn_id;
    args.request = request;
    args.response = response;
    args.request_attachment = std::move(request_attachment);

    auto done = google::protobuf::NewCallback(this, &FastServer::ReturnRPCResponse, args);
    service->CallMethod(method, nullptr, request, response, done);
  }

  void FastServer::ReturnRPCResponse(CallBackArgs args)
  {
    // Use RAII mechanism to release resources.
    std::unique_ptr<google::protobuf::Message> request_guard(args.request);
    std::unique_ptr<google::protobuf::Message> response_guard(args.response);

    uint32_t attachment_len = args.response_attachment.length();
    uint32_t part2_len = args.response->ByteSizeLong();
    uint32_t total_length = fixed_rep_head_bytes + part2_len + attachment_len;

    // Frame header: [|rpc_id|total_len|attachment_size|]
    ResponseHead part1;
    part1.set_rpc_id(args.rpc_id);
    part1.set_total_len(total_length);
    part1.set_attachment_size(attachment_len);
    CHECK(part1.ByteSizeLong() == fixed_rep_head_bytes);

    if (total_length <= max_inline_data)
    {
      // Inline path: build frame directly in stack buffer.
      char msg_buf[max_inline_data];
      char *dst = msg_buf;

      // [|rpc_id|total_len|attachment_size|]
      google::protobuf::io::ArrayOutputStream arr_out0(dst, fixed_rep_head_bytes);
      google::protobuf::io::CodedOutputStream coded0(&arr_out0);
      part1.SerializeWithCachedSizes(&coded0);
      CHECK(!coded0.HadError());
      dst += fixed_rep_head_bytes;

      // Serialize payload directly into msg_buf.
      uint32_t remaining = max_inline_data - (dst - msg_buf);
      google::protobuf::io::ArrayOutputStream arr_out1(dst, remaining);
      CHECK(args.response->SerializeToZeroCopyStream(&arr_out1));
      dst += part2_len;

      // Append response attachment.
      if (attachment_len > 0)
      {
        for (size_t i = 0; i < args.response_attachment.ref_num(); ++i)
        {
          const auto &r = args.response_attachment.ref_at(i);
          memcpy(dst, r.block->data + r.offset, r.length);
          dst += r.length;
        }
      }

      uint64_t msg_addr = reinterpret_cast<uint64_t>(msg_buf);
      SendInlineMessage(args.conn_id->qp,
                        FAST_SmallMessage,
                        msg_addr,
                        total_length);
    }
    else if (total_length < msg_threshold)
    {
      // Medium message: scatter-gather SEND via IOBuf.
      IOBuf resp_frame;

      // Serialize [|rpc_id|total_len|attachment_size|] into IOBuf.
      {
        IOBufAsZeroCopyOutputStream zcos(&resp_frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        part1.SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
      }

      // Serialize payload into IOBuf via ZeroCopyOutputStream.
      {
        IOBufAsZeroCopyOutputStream zcos(&resp_frame);
        google::protobuf::io::CodedOutputStream coded(&zcos);
        args.response->SerializeWithCachedSizes(&coded);
        CHECK(!coded.HadError());
      }

      // Append response attachment.
      if (attachment_len > 0)
      {
        resp_frame.append(args.response_attachment);
      }

      ibv_sge sges[MAX_SGE];
      int sge_count = 0;
      for (size_t i = 0; i < resp_frame.ref_num(); ++i)
      {
        const auto &r = resp_frame.ref_at(i);
        sges[sge_count].addr = reinterpret_cast<uint64_t>(r.block->data) + r.offset;
        sges[sge_count].length = r.length;
        sges[sge_count].lkey = ::fast::GetRegionId(r.block->data);
        ++sge_count;
        ibvsend_server_addrs.push_back(AddressInfo(BLOCK_ADDRESS,
                                                   reinterpret_cast<uint64_t>(r.block->data) + r.offset,
                                                   reinterpret_cast<ibv_mr *>(::fast::GetRegionId(r.block->data)),
                                                   send_counter));
      }

      SendMiddleMessage(args.conn_id->qp, sges, sge_count, FAST_SmallMessage);
    }
    else
      {
        // Large message: two-phase protocol, serialize directly into large block.
        ibv_mr *large_block_mr = shared_rsc_->LargeBlockAlloc(total_length + 1);
        CHECK(large_block_mr != nullptr);
        char *frame_buf = reinterpret_cast<char *>(large_block_mr->addr);

        uint64_t auth_addr;
        shared_rsc_->ObtainOneBlock(auth_addr);
        char *auth_buf = reinterpret_cast<char *>(auth_addr);
        memset(auth_buf + fixed_auth_bytes, '0', 1);

        // Step-1: Send notify message.
        NotifyMessage notify_msg;
        notify_msg.set_total_len(total_length);
        notify_msg.set_remote_key(large_block_mr->lkey);
        notify_msg.set_remote_addr(reinterpret_cast<uint64_t>(large_block_mr->addr));
        CHECK(notify_msg.ByteSizeLong() == fixed_noti_bytes);
        char noti_buf[max_inline_data];
        CHECK(notify_msg.SerializeToArray(noti_buf, fixed_noti_bytes));
        SendInlineMessage(args.conn_id->qp,
                          FAST_NotifyMessage,
                          reinterpret_cast<uint64_t>(noti_buf), fixed_noti_bytes);

        // Step-2: Serialize frame directly into large_block_mr->addr.
        char *dst = frame_buf;

        google::protobuf::io::ArrayOutputStream arr_out0(dst, fixed_rep_head_bytes);
        google::protobuf::io::CodedOutputStream coded0(&arr_out0);
        part1.SerializeWithCachedSizes(&coded0);
        CHECK(!coded0.HadError());
        dst += fixed_rep_head_bytes;

        uint32_t remaining = static_cast<uint32_t>(reinterpret_cast<char *>(large_block_mr->addr) + total_length - dst);
        google::protobuf::io::ArrayOutputStream arr_out1(dst, remaining);
        CHECK(args.response->SerializeToZeroCopyStream(&arr_out1));
        dst += part2_len;

        if (attachment_len > 0)
        {
          memcpy(dst, args.response_attachment.ref_at(0).block->data + args.response_attachment.ref_at(0).offset, attachment_len);
          dst += attachment_len;
        }

        // Set flag byte to '0' (receiver will set '1' when done reading).
        memset(frame_buf + total_length, '0', 1);

        // Step-3: Get authority message from client (busy check).
        volatile char *flag = auth_buf + fixed_auth_bytes;
        while (*flag != '1')
        {
        }
        AuthorityMessage authority_msg;
        CHECK(authority_msg.ParseFromArray(auth_buf, fixed_auth_bytes));
        ::fast::BlockDeallocate(reinterpret_cast<void *>(auth_addr));

        // Step-4: RDMA WRITE the entire frame to receiver's address.
        ibv_sge write_sg;
        write_sg.addr = reinterpret_cast<uint64_t>(large_block_mr->addr);
        write_sg.length = total_length;
        write_sg.lkey = large_block_mr->lkey;

        // Track for WC cleanup.
        ibvsend_server_addrs.push_back(AddressInfo(LARGE_BLOCK_ADDRESS,
                                                   reinterpret_cast<uint64_t>(large_block_mr->addr),
                                                   large_block_mr, send_counter));
        WriteLargeMessage(args.conn_id->qp, &write_sg, 1,
                          authority_msg.remote_key(),
                          authority_msg.remote_addr());
      }
    }

    /// NOTE: Try to poll send CQEs.
    this->TryToPollSendWC(args.conn_id);
  }

} // namespace fast
