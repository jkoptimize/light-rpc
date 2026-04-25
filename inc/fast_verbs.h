#pragma once

#include "inc/fast_define.h"

#include <rdma/rdma_cma.h>

namespace fast
{

    /// @brief Send a message using inline mode (generate a CQE).
    /// This function sets wr_id to 0.
    /// @param qp
    /// @param msg_type This function sets imm_data to msg_type.
    /// @param msg_addr
    /// @param msg_len
    void SendInlineMessage(ibv_qp *qp,
                           MessageType msg_type,
                           uint64_t msg_addr,
                           uint32_t msg_len);

    /// @brief Use the write operation and inline mode to send a message (generate a CQE).
    /// This function sets wr_id to 0.
    /// @param qp
    /// @param msg_addr
    /// @param msg_len
    /// @param remote_key
    /// @param remote_addr
    void WriteInlineMessage(ibv_qp *qp,
                            uint64_t msg_addr,
                            uint32_t msg_len,
                            uint32_t remote_key,
                            uint64_t remote_addr);

    /// @brief Post a scatter-gather RDMA SEND with immediate data.
    ///   Handles selective signaling internally.
    /// @param qp
    /// @param sges Pointer to SGE array.
    /// @param sge_count Number of SGE entries.
    /// @param msg_type Value written to imm_data.
    /// @note Caller is responsible for tracking wr_id / block addresses
    ///   for later resource cleanup (via ibvsend_client_addrs).
    void SendMiddleMessage(ibv_qp *qp,
                           ibv_sge *sges,
                           int sge_count,
                           MessageType msg_type);

    /// @brief Post a scatter-gather RDMA WRITE (all SGEs to the same remote base address).
    ///   Handles selective signaling internally.
    /// @param qp
    /// @param sges Pointer to SGE array.
    /// @param sge_count Number of SGE entries.
    /// @param remote_key
    /// @param remote_addr Base remote address for all SGEs.
    /// @note Caller is responsible for tracking wr_id / block addresses
    ///   for later resource cleanup (via ibvsend_client_addrs).
    void WriteLargeMessage(ibv_qp *qp,
                           ibv_sge *sges,
                           int sge_count,
                           uint32_t remote_key,
                           uint64_t remote_addr);

} // namespace fast