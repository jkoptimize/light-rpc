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

  enum MessageType
  {
    FAST_SmallMessage = 1,
    FAST_NotifyMessage = 2
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