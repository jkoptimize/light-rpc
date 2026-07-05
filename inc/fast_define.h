#pragma once

#include <cstdint>
#include <infiniband/verbs.h>

namespace fast
{

  extern const uint32_t max_inline_data;

  /// @brief Boundary between inline/small and medium messages.
  ///   Used by small-message IOBuf path and recv block sizing.
  extern const uint32_t default_msg_size;
  extern const uint32_t msg_threshold;

  extern const uint32_t fixed32_bytes;
  extern const uint32_t fixed_noti_bytes;
  extern const uint32_t fixed_auth_bytes;
  extern const uint32_t fixed_rep_head_bytes;

  extern const int timeout_in_ms;
  extern const int listen_backlog;

  extern const int cq_poll_min_times;

  constexpr int MAX_SGE = 32;

  enum MessageType : uint32_t
  {
    MSG_NORMAL           = 0, // Inline/Medium RPC 请求
    MSG_NOTIFY           = 1, // Large 传输通知
    MSG_AUTHORITY        = 2, // Large 授权回复
    MSG_NORMAL_RESPONSE  = 3, // RPC 响应
  };

  // 帧头固定大小
  constexpr uint32_t kFrameHeaderBytes = 12;        // total_len + msg_type + rpc_id
  constexpr uint32_t kNotifyFrameBytes  = 16;       // 12 + data_total_len (MSG_NOTIFY 帧)
  constexpr uint32_t kAuthFrameBytes    = 24;       // 12 + rkey + remote_addr (MSG_AUTHORITY 帧)
  constexpr uint32_t kRespHeaderBytes   = 16;       // 12B frame header + 4B error_code

  // Response error codes carried in the error_code field of the response frame.
  enum ErrorCode : uint32_t
  {
    ERR_SUCCESS         = 0,
    ERR_UNKNOWN_SERVICE = 1,
    ERR_UNKNOWN_METHOD  = 2,
    ERR_BAD_REQUEST     = 3,  // meta / payload parse failure on server
    ERR_BAD_RESPONSE    = 4,  // response parse failure on client (set locally)
    ERR_INTERNAL        = 5,
  };

  enum AddressType
  {
    BLOCK_ADDRESS,
    LARGE_BLOCK_ADDRESS // large block allocated via LargeBlockAlloc, needs dereg_mr on free
  };

  struct AddressInfo
  {
    AddressType type;
    uint64_t addr; // block addr for BLOCK_ADDRESS; mr->addr for LARGE_BLOCK_ADDRESS
    ibv_mr *mr;    // valid for LARGE_BLOCK_ADDRESS; nullptr for BLOCK_ADDRESS
    uint64_t send_counter;
    AddressInfo(AddressType t, uint64_t a, ibv_mr *m, uint64_t sc)
        : type(t), addr(a), mr(m), send_counter(sc) {}
  };

} // namespace fast