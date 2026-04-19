#pragma once

#include "inc/fast_unique.h"
#include "inc/fast_utils.h"
#include "inc/fast_iobuf.h"
#include "inc/fast_define.h"
#include "build/fast_impl.pb.h"

#include <boost/asio.hpp>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

namespace fast
{

  /**
   * This class only supports synchronous RPC calls.
   * Therefore, rpc_id is redundant here.
   *
   * IOBuf attachment:
   *   User can attach raw binary data (e.g. file content, mmap region) to a
   *   request via SetRequestAttachment(). The attachment is serialized into the
   *   IOBuf chain alongside header and payload, then sent via scatter-gather
   *   RDMA. The IOBuf data is appended to the RPC message after the protobuf
   *   payload.
   */

  class FastChannel : public google::protobuf::RpcChannel
  {
  public:
    FastChannel(UniqueResource *unique_rsc, std::string dest_ip, int dest_port);
    virtual ~FastChannel();
    virtual void CallMethod(const google::protobuf::MethodDescriptor *method,
                            google::protobuf::RpcController *controller,
                            const google::protobuf::Message *request,
                            google::protobuf::Message *response,
                            google::protobuf::Closure *done) override;

    /// @brief Set request attachment. The IOBuf is moved into the channel,
    ///   cleared after CallMethod returns.
    void SetRequestAttachment(IOBuf &&attachment)
    {
      request_attachment_ = std::move(attachment);
    }

    /// @brief Set request attachment from raw pointer.
    void SetRequestAttachment(const void *data, size_t len)
    {
      request_attachment_.clear();
      request_attachment_.append(data, len);
    }

    /// @brief Get the attachment IOBuf from the response.
    const IOBuf &ResponseAttachment() const { return response_attachment_; }

  private:
    /// @brief Scatter-gather send: extract SGE list from IOBuf and send via RDMA.
    ///   Falls back to copy if IOBuf has too many blocks.
    void SendIOBufMessage(ibv_qp *qp,
                          MessageType msg_type,
                          const IOBuf &msg,
                          uint32_t total_length,
                          uint32_t lkey);

    void TryToPollSendWC();
    void ProcessSendWorkCompletion(ibv_wc &send_wc);
    ibv_mr *ProcessNotifyMessage(uint64_t block_addr);
    void ParseAndProcessResponse(void *addr, bool small_msg,
                                 google::protobuf::Message *response);

    UniqueResource *unique_rsc_;
    uint32_t rpc_id_; // 1

    /// @brief Attachment provided by user for the next RPC.
    ///   Cleared after CallMethod returns.
    IOBuf request_attachment_;

    /// @brief Attachment parsed from response.
    IOBuf response_attachment_;
  };

} // namespace fast
