#include "fast_large_block.h"
#include "fast_log.h"
#include "fast_utils.h"

#include <cstdlib>

namespace fast {

extern ibv_pd* g_pd;

static const size_t kMaxCachedLargeBlocks = 8;
static __thread LargeBlockNode* tls_large_block_list = nullptr;
static __thread size_t tls_large_block_num = 0;
static __thread bool tls_large_inited = false;

static void RecycleTLSLargeBlocks() {
  while (tls_large_block_list != nullptr) {
    LargeBlockNode* node = tls_large_block_list;
    tls_large_block_list = node->next;
    CHECK(ibv_dereg_mr(node->mr) == 0);
    free(node->buf);
    delete node;
  }
  tls_large_block_num = 0;
}

ibv_mr* LargeBlockAlloc(size_t size) {
  if (!tls_large_inited) {
    tls_large_inited = true;
    ThreadExitHelper::add_callback([] { RecycleTLSLargeBlocks(); });
  }

  // First-fit = best-fit on ascending-size list.
  LargeBlockNode* prev = nullptr;
  LargeBlockNode* cur = tls_large_block_list;
  while (cur != nullptr && cur->size < size) {
    prev = cur;
    cur = cur->next;
  }
  if (cur != nullptr) {
    // Unlink cur from the list.
    if (prev != nullptr) {
      prev->next = cur->next;
    } else {
      tls_large_block_list = cur->next;
    }
    tls_large_block_num--;
    ibv_mr* mr = cur->mr;
    delete cur;
    return mr;
  }

  // Fallback: allocate + register.
  void* buf = nullptr;
  if (posix_memalign(&buf, 4096, size) != 0) {
    LOG(ERROR) << "LargeBlockAlloc: posix_memalign failed";
    return nullptr;
  }
  ibv_mr* mr = ibv_reg_mr(g_pd, buf, size,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (mr == nullptr) {
    LOG(ERROR) << "LargeBlockAlloc: ibv_reg_mr failed";
    free(buf);
    return nullptr;
  }
  return mr;
}

void ReturnLargeBlock(ibv_mr* mr) {
  if (!mr)
    return;

  if (!tls_large_inited) {
    tls_large_inited = true;
    ThreadExitHelper::add_callback([] { RecycleTLSLargeBlocks(); });
  }

  // Best-fit insert: keep list sorted by size (ascending).
  size_t size = mr->length;
  LargeBlockNode* node = new LargeBlockNode{mr->addr, size, mr, nullptr};
  if (tls_large_block_list == nullptr || size < tls_large_block_list->size) {
    node->next = tls_large_block_list;
    tls_large_block_list = node;
  } else {
    LargeBlockNode* cur = tls_large_block_list;
    while (cur->next != nullptr && cur->next->size <= size)
      cur = cur->next;
    node->next = cur->next;
    cur->next = node;
  }
  tls_large_block_num++;
  if (tls_large_block_num > kMaxCachedLargeBlocks) {
    // Evict largest block (sorted by size)
    LargeBlockNode* last = tls_large_block_list;
    while (last->next != nullptr && last->next->next != nullptr)
      last = last->next;
    LargeBlockNode* to_free = last->next;
    last->next = nullptr;
    CHECK(ibv_dereg_mr(to_free->mr) == 0);
    free(to_free->buf);
    delete to_free;
    tls_large_block_num--;
  }
}

// For unit tests only
LargeBlockNode* LargeBlockFindBestFit(LargeBlockNode* head, size_t size) {
  // First-fit = best-fit on ascending-size list.
  LargeBlockNode* cur = head;
  while (cur != nullptr && cur->size < size) {
    cur = cur->next;
  }
  return cur;  // first node with size >= request, or nullptr

}

// For unit tests only
LargeBlockNode* LargeBlockInsertSorted(LargeBlockNode* head,
                                       LargeBlockNode* node) {
  if (head == nullptr || node->size < head->size) {
    node->next = head;
    return node;
  }
  LargeBlockNode* cur = head;
  while (cur->next != nullptr && cur->next->size <= node->size)
    cur = cur->next;
  node->next = cur->next;
  cur->next = node;
  return head;
}

// For unit tests only
LargeBlockNode* LargeBlockPopLast(LargeBlockNode* head) {
  if (head == nullptr || head->next == nullptr)
    return nullptr;
  LargeBlockNode* cur = head;
  while (cur->next->next != nullptr)
    cur = cur->next;
  LargeBlockNode* last = cur->next;
  cur->next = nullptr;
  return last;
}

}  // namespace fast
