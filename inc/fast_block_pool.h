#ifndef FAST_BLOCK_POOL_H
#define FAST_BLOCK_POOL_H

#include <infiniband/verbs.h>
#include <string>
#include <vector>
#include <mutex>
#include <functional>

namespace fast
{
    typedef uint32_t (*RegisterCallback)(void *, size_t);
    bool InitBlockPool(RegisterCallback cb);
    void *BlockAllocate(size_t len);
    void BlockDeallocate(void *buf);
    uint32_t GetRegionId(const void *buf);
    size_t GetBlockSize(int type);
}

#endif // FAST_BLOCK_POOL_H
