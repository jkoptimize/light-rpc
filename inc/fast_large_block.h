#pragma once
#include <infiniband/verbs.h>
#include <cstddef>

namespace fast {

struct LargeBlockNode {
  void *buf;
  size_t size;
  ibv_mr *mr;
  LargeBlockNode *next;
};

ibv_mr* LargeBlockAlloc(size_t size);
void ReturnLargeBlock(ibv_mr* mr);

// For unit tests only
LargeBlockNode* LargeBlockFindBestFit(LargeBlockNode* head, size_t size);
// For unit tests only
LargeBlockNode* LargeBlockInsertSorted(LargeBlockNode* head, LargeBlockNode* node);
// For unit tests only
LargeBlockNode* LargeBlockPopLast(LargeBlockNode* head);

}  // namespace fast
