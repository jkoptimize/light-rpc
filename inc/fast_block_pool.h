#ifndef FAST_BLOCK_POOL_H
#define FAST_BLOCK_POOL_H

#include <infiniband/verbs.h>
#include <string>
#include <vector>
#include <mutex>
#include <functional>

namespace fast
{
    void SetGlobalPD(ibv_pd *pd);
    bool InitBlockPool();
    void *BlockAllocate(size_t len);
    void BlockDeallocate(void *buf);

    /// @brief Get the lkey for the region containing buf.
    uint32_t GetRegionId(const void *buf);

    size_t GetBlockSize(int type);
}

#endif // FAST_BLOCK_POOL_H
