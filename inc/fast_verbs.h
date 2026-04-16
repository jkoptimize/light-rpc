#pragma once

#include "inc/fast_define.h"
#include "inc/fast_iobuf.h"

#include <rdma/rdma_cma.h>

namespace fast {

constexpr int MAX_SGE = 32;

/// @brief Send a message using inline mode (generate a CQE).
/// This function sets wr_id to 0.
/// @param qp
/// @param msg_type This function sets imm_data to msg_type.
/// @param msg_addr
/// @param msg_len
void SendInlineMessage(ibv_qp* qp,
                       MessageType msg_type,
                       uint64_t msg_addr,
                       uint32_t msg_len);

/// @brief Send a small message (generate a CQE).
/// @param qp
/// @param msg_type This function sets imm_data to msg_type.
/// @param msg_addr This function sets wr_id to msg_addr (block address).
/// @param msg_len
/// @param lkey
void SendSmallMessage(ibv_qp* qp,
                      MessageType msg_type,
                      uint64_t msg_addr,
                      uint32_t msg_len,
                      uint32_t lkey);

/// @brief Use the write operation and inline mode to send a message (generate a CQE).
/// This function sets wr_id to 0.
/// @param qp
/// @param msg_addr
/// @param msg_len
/// @param remote_key
/// @param remote_addr
void WriteInlineMessage(ibv_qp* qp,
                        uint64_t msg_addr,
                        uint32_t msg_len,
                        uint32_t remote_key,
                        uint64_t remote_addr);

/// @brief Use the write operation to send a large message (generate a CQE).
/// @param qp
/// @param msg_mr This function sets wr_id to the MR address.
/// @param msg_len The length of MR may be greater than the msg_len.
/// @param remote_key
/// @param remote_addr
void WriteLargeMessage(ibv_qp* qp,
                       ibv_mr* msg_mr,
                       uint32_t msg_len,
                       uint32_t remote_key,
                       uint64_t remote_addr);

/// @brief Scatter-gather RDMA send: extract SGE list from IOBuf and post
///   a single WR with multiple SGE entries.
/// @param qp
/// @param msg_type imm_data sent with the WR.
/// @param msg The IOBuf to send. Its blocks must be in registered RDMA memory
///   (e.g. block_pool blocks). Each BlockRef becomes one SGE entry.
/// @param total_length Total bytes to send.
/// @param lkey lkey of the RDMA memory region.
/// @return Number of SGE entries used, or -1 on error.
int SendScatterGatherMessage(ibv_qp* qp,
                             MessageType msg_type,
                             const IOBuf& msg,
                             uint32_t total_length,
                             uint32_t lkey);

/// @brief Scatter-gather RDMA write: extract SGE list from IOBuf and post
///   a single WR with multiple SGE entries.
/// @param qp
/// @param msg The IOBuf to write. Its blocks must be in registered RDMA memory.
/// @param total_length Total bytes to write.
/// @param lkey lkey of the RDMA memory region.
/// @param remote_key
/// @param remote_addr
/// @return Number of SGE entries used, or -1 on error.
int WriteScatterGatherMessage(ibv_qp* qp,
                              const IOBuf& msg,
                              uint32_t total_length,
                              uint32_t lkey,
                              uint32_t remote_key,
                              uint64_t remote_addr);

} // namespace fast