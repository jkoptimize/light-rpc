#include <gtest/gtest.h>
#include "fast_large_block.h"

using namespace fast;

// ============================================================
// LargeBlockFindBestFit
// ============================================================

TEST(LargeBlockFindBestFit, EmptyList) {
  EXPECT_EQ(LargeBlockFindBestFit(nullptr, 100), nullptr);
}

TEST(LargeBlockFindBestFit, SingleNode_HeadCannotBeFound) {
  LargeBlockNode n1{nullptr, 100, nullptr, nullptr};
  // Head (first node) can never be found by design -- the search logic
  // only returns nodes at position >= 1 (i.e., non-head nodes).
  EXPECT_EQ(LargeBlockFindBestFit(&n1, 100), nullptr);
}

TEST(LargeBlockFindBestFit, SecondNodeExactMatch) {
  LargeBlockNode n1{nullptr, 10, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 100, nullptr, nullptr};
  n1.next = &n2;
  // Head (10) is too small, second node (100) matches exactly.
  EXPECT_EQ(LargeBlockFindBestFit(&n1, 100), &n2);
}

TEST(LargeBlockFindBestFit, BestFitSelectsSmallestExcess) {
  LargeBlockNode n1{nullptr, 10, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 50, nullptr, nullptr};
  LargeBlockNode n3{nullptr, 100, nullptr, nullptr};
  n1.next = &n2;
  n2.next = &n3;
  // Target 60: n3(100) has diff=40, n2(50) is too small.
  EXPECT_EQ(LargeBlockFindBestFit(&n1, 60), &n3);
}

TEST(LargeBlockFindBestFit, BestFitWithMultipleCandidates) {
  LargeBlockNode n1{nullptr, 10, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 60, nullptr, nullptr};
  LargeBlockNode n3{nullptr, 80, nullptr, nullptr};
  n1.next = &n2;
  n2.next = &n3;
  // Target 50: 60-50=10, 80-50=30 => 60 is best.
  EXPECT_EQ(LargeBlockFindBestFit(&n1, 50), &n2);
}

TEST(LargeBlockFindBestFit, NoSuitableNode) {
  LargeBlockNode n1{nullptr, 10, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 20, nullptr, nullptr};
  n1.next = &n2;
  EXPECT_EQ(LargeBlockFindBestFit(&n1, 200), nullptr);
}

TEST(LargeBlockFindBestFit, ExactMatchInMiddle) {
  LargeBlockNode n1{nullptr, 5, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 50, nullptr, nullptr};
  LargeBlockNode n3{nullptr, 100, nullptr, nullptr};
  n1.next = &n2;
  n2.next = &n3;
  EXPECT_EQ(LargeBlockFindBestFit(&n1, 50), &n2);
}

// ============================================================
// LargeBlockInsertSorted
// ============================================================

TEST(LargeBlockInsertSorted, InsertIntoEmptyList) {
  LargeBlockNode node{nullptr, 100, nullptr, nullptr};
  LargeBlockNode* head = LargeBlockInsertSorted(nullptr, &node);
  EXPECT_EQ(head, &node);
  EXPECT_EQ(head->next, nullptr);
}

TEST(LargeBlockInsertSorted, InsertAtFront) {
  LargeBlockNode n1{nullptr, 100, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 50, nullptr, nullptr};
  n1.next = nullptr;
  LargeBlockNode* head = LargeBlockInsertSorted(&n1, &n2);
  EXPECT_EQ(head, &n2);
  EXPECT_EQ(head->next, &n1);
  EXPECT_EQ(n1.next, nullptr);
}

TEST(LargeBlockInsertSorted, InsertAtEnd) {
  LargeBlockNode n1{nullptr, 50, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 100, nullptr, nullptr};
  n1.next = nullptr;
  LargeBlockNode* head = LargeBlockInsertSorted(&n1, &n2);
  EXPECT_EQ(head, &n1);
  EXPECT_EQ(head->next, &n2);
  EXPECT_EQ(n2.next, nullptr);
}

TEST(LargeBlockInsertSorted, InsertInMiddle) {
  LargeBlockNode n1{nullptr, 50, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 50, nullptr, nullptr};
  LargeBlockNode n3{nullptr, 100, nullptr, nullptr};
  n1.next = &n3;
  n3.next = nullptr;
  LargeBlockNode* head = LargeBlockInsertSorted(&n1, &n2);
  // 50 == 50: n2 goes after n1 but before n3.
  EXPECT_EQ(head, &n1);
  EXPECT_EQ(n1.next, &n2);
  EXPECT_EQ(n2.next, &n3);
  EXPECT_EQ(n3.next, nullptr);
}

TEST(LargeBlockInsertSorted, InsertMaintainsSortedOrder) {
  LargeBlockNode n1{nullptr, 10, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 10, nullptr, nullptr};
  LargeBlockNode n3{nullptr, 30, nullptr, nullptr};
  n1.next = nullptr;
  LargeBlockNode* head = LargeBlockInsertSorted(&n1, &n3);
  head = LargeBlockInsertSorted(head, &n2);
  EXPECT_EQ(head, &n1);
  EXPECT_EQ(head->size, 10u);
  EXPECT_EQ(head->next->size, 10u);
  EXPECT_EQ(head->next->next->size, 30u);
  EXPECT_EQ(head->next->next->next, nullptr);
}

// ============================================================
// LargeBlockPopLast
// ============================================================

TEST(LargeBlockPopLast, EmptyList) {
  EXPECT_EQ(LargeBlockPopLast(nullptr), nullptr);
}

TEST(LargeBlockPopLast, SingleElement) {
  LargeBlockNode n1{nullptr, 100, nullptr, nullptr};
  EXPECT_EQ(LargeBlockPopLast(&n1), nullptr);
}

TEST(LargeBlockPopLast, TwoElements) {
  LargeBlockNode n1{nullptr, 50, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 100, nullptr, nullptr};
  n1.next = &n2;
  LargeBlockNode* removed = LargeBlockPopLast(&n1);
  EXPECT_EQ(removed, &n2);
  EXPECT_EQ(n1.next, nullptr);
}

TEST(LargeBlockPopLast, MultipleElements) {
  LargeBlockNode n1{nullptr, 10, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 20, nullptr, nullptr};
  LargeBlockNode n3{nullptr, 30, nullptr, nullptr};
  n1.next = &n2;
  n2.next = &n3;
  LargeBlockNode* removed = LargeBlockPopLast(&n1);
  EXPECT_EQ(removed, &n3);
  EXPECT_EQ(n1.next, &n2);
  EXPECT_EQ(n2.next, nullptr);
}

TEST(LargeBlockPopLast, DoublePop) {
  LargeBlockNode n1{nullptr, 10, nullptr, nullptr};
  LargeBlockNode n2{nullptr, 20, nullptr, nullptr};
  n1.next = &n2;

  LargeBlockNode* r1 = LargeBlockPopLast(&n1);
  EXPECT_EQ(r1, &n2);
  EXPECT_EQ(n1.next, nullptr);

  EXPECT_EQ(LargeBlockPopLast(&n1), nullptr);
}
