#include <mutex>
#include "fast_block_pool.h"
#include "fast_log.h"
#include "fast_utils.h"

namespace fast
{
    static const size_t RDMA_INITIAL_REGION_SIZE = 1024;  // 初始region大小，单位mb
    static const size_t RDMA_MEMPOOL_BUCKETS = 4;         // 每个region的分区(bucket)数量
    static const size_t RDMA_MEMPOOL_MAX_REGIONS = 16;    // 最大region数量
    static const size_t RDMA_MEMPOOL_TLS_CACHE_NUM = 128; // 每个线程TLS链表的缓存最大数量

    static const int BLOCK_DEFAULT = 0;    // 8KB
    static const int BLOCK_SIZE_COUNT = 3; // 总计三种类型Block:8KB, 64KB, 2MB
    static const size_t BYTES_IN_MB = 1048576;
    static size_t g_block_size[BLOCK_SIZE_COUNT] = {8192, 65536, 2 * BYTES_IN_MB};
    static ibv_pd *g_pd = NULL;           // 全局 pd for block pool memory registration
    static size_t g_region_num = 0;        // 已经分配的region数量
    // Forward declaration for TLS pointer
    struct IdleNode;
    // Only for default block size
    static __thread IdleNode *tls_idle_list = NULL; // TLS空闲链表，默认block大小的region分配的block会先放入TLS链表，优先从TLS链表分配，减少锁竞争
    static __thread size_t tls_idle_num = 0;
    static __thread bool tls_inited = false;

    struct IdleNode
    {
        void *start;
        size_t len;
        IdleNode *next;
    };
    struct Region
    {
        Region() { start = 0; }
        uintptr_t start;
        size_t size;
        uint32_t block_type;
        uint32_t id; // lkey
    };
    static std::vector<ibv_mr *> *g_mrs = NULL;        // 注册的mr
    static Region g_regions[RDMA_MEMPOOL_MAX_REGIONS]; // 全局region, 每种类型的block分配一个region, 最大支持16块region

    struct GlobalInfo
    {
        std::vector<IdleNode *> idle_list[BLOCK_SIZE_COUNT];
        std::vector<std::mutex *> lock[BLOCK_SIZE_COUNT];
        std::vector<size_t> idle_size[BLOCK_SIZE_COUNT];
        int region_num[BLOCK_SIZE_COUNT];
        std::mutex extend_lock;
        std::vector<IdleNode *> expansion_list[BLOCK_SIZE_COUNT];
        std::vector<size_t> expansion_size[BLOCK_SIZE_COUNT];
    };
    static GlobalInfo *g_info = NULL;

    // Forward declarations for functions used before definition
    static Region *GetRegion(const void *buf);
    static void *ExtendBlockPool(size_t region_size, int block_type);
    static void *ExtendBlockPoolImpl(void *region_base, size_t region_size, int block_type);
    static void *AllocBlockFrom(int block_type);
    static void MoveExpansionList2EmptyIdleList(int block_type, size_t index);

    size_t GetBlockSize(int type)
    {
        return g_block_size[type];
    }

    uint32_t GetRegionId(const void *buf)
    {
        Region *r = GetRegion(buf);
        if (!r)
        {
            return 0;
        }
        return r->id;
    }

    static inline Region *GetRegion(const void *buf)
    {
        if (!buf)
        {
            return NULL;
        }
        Region *r = NULL;
        uintptr_t addr = (uintptr_t)buf;
        for (int i = 0; i < RDMA_MEMPOOL_MAX_REGIONS; ++i)
        {
            if (g_regions[i].start == 0)
            {
                break;
            }
            if (addr >= g_regions[i].start &&
                addr < g_regions[i].start + g_regions[i].size)
            {
                r = &g_regions[i];
                break;
            }
        }
        return r;
    }

    uint32_t RdmaRegisterMemory(struct ibv_pd *pd, void *buf, size_t size)
    {
        // Register the memory as callback in block_pool
        // The thread-safety should be guaranteed by the caller
        ibv_mr *mr = ibv_reg_mr(pd, buf, size, IBV_ACCESS_LOCAL_WRITE);
        if (!mr)
        {
            LOG(ERROR) << "Fail to register memory";
            return 0;
        }
        g_mrs->push_back(mr);
        return mr->lkey;
    }

    void SetGlobalPD(ibv_pd *pd) {
        g_pd = pd;
    }

    bool InitBlockPool()
    {
        if (g_pd == NULL)
        {
            LOG(ERROR) << "Block pool PD not set. Call SetGlobalPD first.";
            return false;
        }
        if (g_region_num > 0)
        {
            LOG(WARNING) << "Block pool has already been initialized.";
            return false;
        }
        for (int i = 0; i < BLOCK_SIZE_COUNT; i++)
        {
            g_info->idle_list[i].resize(RDMA_MEMPOOL_BUCKETS, nullptr);
            if (g_info->idle_list[i].size() != RDMA_MEMPOOL_BUCKETS)
            {
                LOG(ERROR) << "Fail to resize block pool[idle_list].";
                return false;
            }
            g_info->idle_size[i].resize(RDMA_MEMPOOL_BUCKETS, 0);
            if (g_info->idle_size[i].size() != RDMA_MEMPOOL_BUCKETS)
            {
                LOG(ERROR) << "Fail to resize block pool[idle_size].";
                return false;
            }
            g_info->lock[i].resize(RDMA_MEMPOOL_BUCKETS, nullptr);
            if (g_info->lock[i].size() != RDMA_MEMPOOL_BUCKETS)
            {
                LOG(ERROR) << "Fail to resize block pool[mutex].";
                return false;
            }
            g_info->region_num[i] = 0;
            for (size_t j = 0; j < RDMA_MEMPOOL_BUCKETS; j++)
            {
                g_info->lock[i][j] = new std::mutex();
                if (!g_info->lock[i][j])
                {
                    LOG(ERROR) << "Fail to initialize block pool[mutex].";
                    return false;
                }
            }
            g_info->expansion_list[i].resize(RDMA_MEMPOOL_BUCKETS, nullptr);
            if (g_info->expansion_list[i].size() != RDMA_MEMPOOL_BUCKETS)
            {
                LOG(ERROR) << "Fail to resize block pool[expansion_list].";
                return false;
            }
            g_info->expansion_size[i].resize(RDMA_MEMPOOL_BUCKETS, 0);
            if (g_info->expansion_size[i].size() != RDMA_MEMPOOL_BUCKETS)
            {
                LOG(ERROR) << "Fail to resize block pool[expansion_size].";
                return false;
            }
        }
        if (ExtendBlockPool(RDMA_INITIAL_REGION_SIZE, BLOCK_DEFAULT))
        {
            return true;
        }

        return false;
    }

    static void *ExtendBlockPool(size_t region_size, int block_type)
    {
        if (region_size < 1)
        {
            LOG(ERROR) << "Invalid region_size: " << region_size;
            return NULL;
        }
        region_size = region_size * BYTES_IN_MB / g_block_size[block_type] / RDMA_MEMPOOL_BUCKETS;
        region_size *= g_block_size[block_type] * RDMA_MEMPOOL_BUCKETS;
        LOG(INFO) << "Start extend rdma memory " << region_size / BYTES_IN_MB << "MB";
        void *region_base = NULL;
        if (posix_memalign(&region_base, 4096, region_size) != 0)
        {
            LOG(ERROR) << "Memory not enough";
            return NULL;
        }
        return ExtendBlockPoolImpl(region_base, region_size, block_type);
    }

    static void *ExtendBlockPoolImpl(void *region_base, size_t region_size, int block_type)
    {
        auto region_base_guard = fast::MakeScopeGuard([&]()
                                                      { free(region_base); });
        if (g_region_num == RDMA_MEMPOOL_MAX_REGIONS)
        {
            LOG(ERROR) << "Exceed the maximum number of regions: " << RDMA_MEMPOOL_MAX_REGIONS;
            return NULL;
        }
        uint32_t id = RdmaRegisterMemory(g_pd, region_base, region_size);
        if (id == 0)
        {
            LOG(ERROR) << "Fail to register memory for block pool.";
            return NULL;
        }

        IdleNode *node[RDMA_MEMPOOL_BUCKETS];
        for (int i = 0; i < RDMA_MEMPOOL_BUCKETS; i++)
        {
            node[i] = new IdleNode();
            if (!node[i])
            {
                LOG(ERROR) << "Fail to allocate idle node.";
                for (int j = 0; j < i; j++)
                {
                    delete node[j];
                }
                return NULL;
            }
        }

        Region *region = &g_regions[g_region_num++];
        region->start = (uintptr_t)region_base;
        region->size = region_size;
        region->id = id;
        region->block_type = block_type;
        for (size_t i = 0; i < RDMA_MEMPOOL_BUCKETS; ++i)
        {
            node[i]->start = (void *)(region->start + i * (region_size / RDMA_MEMPOOL_BUCKETS));
            node[i]->len = region_size / RDMA_MEMPOOL_BUCKETS;
            node[i]->next = g_info->expansion_list[block_type][i];
            g_info->expansion_list[block_type][i] = node[i];
            g_info->expansion_size[block_type][i] += node[i]->len;
        }
        g_info->region_num[block_type]++;
        region_base_guard.dismiss();
        return region_base;
    }

    void *BlockAllocate(size_t len)
    {
        if (len == 0)
        {
            return NULL;
        }
        if (len == 0 || len > g_block_size[BLOCK_SIZE_COUNT - 1])
        {
            return NULL;
        }
        void *ptr = nullptr;
        for (int i = 0; i < BLOCK_SIZE_COUNT; ++i)
        {
            if (len <= g_block_size[i])
            {
                ptr = AllocBlockFrom(i);
                break;
            }
        }
        if (ptr != nullptr)
        {
            return ptr;
        }

        LOG(ERROR) << "Fail to get block from memory pool";
        return nullptr;
    }

    static void *AllocBlockFrom(int block_type)
    {
        void *ptr = nullptr;
        if (block_type == 0 && tls_idle_list)
        {
            IdleNode *node = tls_idle_list;
            tls_idle_list = node->next;
            ptr = node->start;
            delete node;
            tls_idle_num--;
            return ptr;
        }

        size_t index = std::rand() % RDMA_MEMPOOL_BUCKETS;
        std::lock_guard<std::mutex> guard(*g_info->lock[block_type][index]);
        IdleNode *node = g_info->idle_list[block_type][index];
        if (node == nullptr)
        {
            std::lock_guard<std::mutex> guard(g_info->extend_lock);
            node = g_info->idle_list[block_type][index];
            if (node == nullptr && g_info->expansion_list[block_type][index] != nullptr)
            {
                MoveExpansionList2EmptyIdleList(block_type, index);
                node = g_info->idle_list[block_type][index];
            }
            if (node == nullptr)
            {
                if (!ExtendBlockPool(RDMA_INITIAL_REGION_SIZE, block_type))
                {
                    LOG(ERROR) << "Fail to extend block pool.";
                    return nullptr;
                }
                MoveExpansionList2EmptyIdleList(block_type, index);
                node = g_info->idle_list[block_type][index];
            }
        }
        CHECK(node != nullptr);

        ptr = node->start;
        if (node->len > g_block_size[block_type])
        {
            node->start = (void *)((uintptr_t)node->start + g_block_size[block_type]);
            node->len -= g_block_size[block_type];
        }
        else
        {
            g_info->idle_list[block_type][index] = node->next;
            delete node;
        }
        g_info->idle_size[block_type][index] -= node->len;

        // Move more blocks from global list to tls list
        // 此时，如果block_type为0，则tls_idle_list一定为空，需要移动一部分block到tls链表
        if (block_type == 0)
        {
            node = g_info->idle_list[block_type][index];
            tls_idle_list = node;
            IdleNode *last_node = node;
            while (node)
            {
                if (tls_idle_num >= RDMA_MEMPOOL_TLS_CACHE_NUM / 2 || node->len > g_block_size[block_type])
                {
                    break;
                }
                tls_idle_num++;
                last_node = node;
                node = node->next;
            }
            if (tls_idle_num == 0)
            {
                tls_idle_list = nullptr;
            }
            else
            {
                g_info->idle_list[block_type][index] = node;
            }
            if (last_node)
            {
                last_node->next = nullptr;
            }
        }
        return ptr;
    }

    static void MoveExpansionList2EmptyIdleList(int block_type, size_t index)
    {
        CHECK(NULL == g_info->idle_list[block_type][index]);

        g_info->idle_list[block_type][index] = g_info->expansion_list[block_type][index];
        g_info->idle_size[block_type][index] += g_info->expansion_size[block_type][index];
        g_info->expansion_list[block_type][index] = NULL;
        g_info->expansion_size[block_type][index] = 0;
    }

    void RecycleAll()
    {
        // Only block_type == 0 needs recycle
        while (tls_idle_list)
        {
            IdleNode *node = tls_idle_list;
            tls_idle_list = node->next;
            Region *r = GetRegion(node->start);
            if (!r)
            {
                continue;
            }
            uint64_t index = ((uintptr_t)node->start - r->start) * RDMA_MEMPOOL_BUCKETS / r->size;
            std::lock_guard<std::mutex> guard(*g_info->lock[0][index]);
            node->next = g_info->idle_list[0][index];
            g_info->idle_list[0][index] = node;
        }
        tls_idle_num = 0;
    }

    void BlockDeallocate(void *buf)
    {
        if (!buf)
        {
            return;
        }

        Region *r = GetRegion(buf);
        if (!r)
        {
            errno = ERANGE;
            return;
        }
        IdleNode *node = new IdleNode();
        if (!node)
        {
            LOG(ERROR) << "Fail to allocate idle node.";
            return;
        }

        uint32_t block_type = r->block_type;
        size_t block_size = g_block_size[block_type];
        node->start = buf;
        node->len = block_size;
        if (block_type == 0 && tls_idle_num < RDMA_MEMPOOL_TLS_CACHE_NUM)
        {
            if (!tls_inited)
            {
                tls_inited = true;
                ThreadExitHelper::add_callback([]()
                                               { RecycleAll(); });
            }
            node->next = tls_idle_list;
            tls_idle_list = node;
            tls_idle_num++;
            return;
        }

        uint64_t index = ((uintptr_t)buf - r->start) * RDMA_MEMPOOL_BUCKETS / r->size;
        // block为4k时，如果超过了tls缓存数量限制，将一半的TLS block还给全局链表中; 其他类型则直接归还到全局链表
        if (block_type == 0)
        {
            size_t cnt = RDMA_MEMPOOL_TLS_CACHE_NUM / 2;
            size_t len = 0;
            IdleNode *new_head = tls_idle_list;
            IdleNode *last_node = nullptr;
            for (int i = 0; i < cnt; i++)
            {
                len += new_head->len;
                last_node = new_head;
                new_head = new_head->next;
            }
            if (last_node)
            {
                std::lock_guard<std::mutex> guard(*g_info->lock[block_type][index]);
                last_node->next = node;
                node->next = g_info->idle_list[block_type][index];
                g_info->idle_list[block_type][index] = tls_idle_list;
                g_info->idle_size[block_type][index] += len;
            }
            tls_idle_list = new_head;
            tls_idle_num -= cnt;
        }
        else
        {
            std::lock_guard<std::mutex> guard(*g_info->lock[block_type][index]);
            node->next = g_info->idle_list[block_type][index];
            g_info->idle_size[block_type][index] += node->len;
            g_info->idle_list[block_type][index] = node;
        }
        return;
    }

} // namespace fast