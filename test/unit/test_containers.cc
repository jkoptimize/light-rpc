#include <gtest/gtest.h>
#include <stdint.h>
#include <vector>

#include "butil/flat_map.h"
#include "butil/linked_list.h"
#include "butil/bounded_queue.h"
#include "butil/resource_pool.h"
#include "butil/object_pool.h"

namespace fast {
namespace butil {
namespace {

// ---- linked_list ----

struct IntNode : public LinkNode<IntNode> {
    explicit IntNode(int v) : value(v) {}
    int value;
};

TEST(LinkedListTest, AppendAndIterate) {
    LinkedList<IntNode> list;
    IntNode n1(1), n2(2), n3(3);
    list.Append(&n1);
    list.Append(&n2);
    list.Append(&n3);

    std::vector<int> seen;
    for (LinkNode<IntNode>* p = list.head(); p != list.end(); p = p->next()) {
        seen.push_back(p->value()->value);
    }
    ASSERT_EQ(3u, seen.size());
    EXPECT_EQ(1, seen[0]);
    EXPECT_EQ(2, seen[1]);
    EXPECT_EQ(3, seen[2]);
}

TEST(LinkedListTest, RemoveFromList) {
    LinkedList<IntNode> list;
    IntNode n1(1), n2(2);
    list.Append(&n1);
    list.Append(&n2);
    n1.RemoveFromList();
    EXPECT_TRUE(list.head() == &n2);
    EXPECT_TRUE(list.head()->next() == list.end());
}

// ---- bounded_queue ----

TEST(BoundedQueueTest, PushPop) {
    BoundedQueue<int> q(4);
    ASSERT_TRUE(q.initialized());
    EXPECT_TRUE(q.push(10));
    EXPECT_TRUE(q.push(20));
    int v = 0;
    EXPECT_TRUE(q.pop(&v));
    EXPECT_EQ(10, v);
    EXPECT_TRUE(q.pop(&v));
    EXPECT_EQ(20, v);
    EXPECT_TRUE(q.empty());
}

TEST(BoundedQueueTest, Full) {
    BoundedQueue<int> q(2);
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_FALSE(q.push(3));  // full
    EXPECT_TRUE(q.full());
}

// ---- flat_map ----

TEST(FlatMapTest, InitInsertSeek) {
    FlatMap<int, int> m;
    ASSERT_EQ(0, m.init(8));
    m[1] = 10;
    m[2] = 20;
    EXPECT_EQ(2u, m.size());
    EXPECT_EQ(10, *m.seek(1));
    EXPECT_EQ(20, *m.seek(2));
    EXPECT_EQ(nullptr, m.seek(3));
}

TEST(FlatMapTest, OperatorBracketDefault) {
    FlatMap<int, int> m;
    ASSERT_EQ(0, m.init(4));
    EXPECT_EQ(0, m[5]);  // default-constructed
}

TEST(FlatMapTest, Iterate) {
    FlatMap<int, int> m;
    ASSERT_EQ(0, m.init(4));
    m[1] = 100;
    m[2] = 200;
    int sum = 0;
    for (auto it = m.begin(); it != m.end(); ++it) {
        sum += it->second;
    }
    EXPECT_EQ(300, sum);
}

// Mirrors butex_wake_n's per-tag TaskGroup dedup: FlatMap<int, T*>.
TEST(FlatMapTest, PointerValue) {
    FlatMap<int, int*> m;
    ASSERT_EQ(0, m.init(4));
    int a = 1, b = 2;
    m[0] = &a;
    m[1] = &b;
    m[0] = &a;  // overwrite, no new entry
    EXPECT_EQ(2u, m.size());
    EXPECT_EQ(&a, m[0]);
    EXPECT_EQ(&b, m[1]);
}

TEST(FlatMapTest, Erase) {
    FlatMap<int, int> m;
    ASSERT_EQ(0, m.init(8));
    m[1] = 10;
    m[2] = 20;
    EXPECT_EQ(1u, m.erase(1));
    EXPECT_EQ(0u, m.erase(1));  // already gone
    EXPECT_EQ(1u, m.size());
    EXPECT_EQ(nullptr, m.seek(1));
}

// Forces load-factor driven resize (default 16 buckets, load_factor 80).
// Every inserted key must survive the power-of-2 rehash.
TEST(FlatMapTest, Resize) {
    FlatMap<int, int> m;
    ASSERT_EQ(0, m.init(16));
    const int N = 100;
    for (int i = 0; i < N; ++i) {
        m[i] = i * 10;
    }
    EXPECT_EQ(static_cast<size_t>(N), m.size());
    EXPECT_GT(m.bucket_count(), 16u);
    for (int i = 0; i < N; ++i) {
        ASSERT_NE(nullptr, m.seek(i));
        EXPECT_EQ(i * 10, *m.seek(i));
    }
}

// ---- resource_pool ----

struct ResNode {
    ResNode() : value(0) {}
    int value;
};

TEST(ResourcePoolTest, GetAddressReturn) {
    ResourceId<ResNode> id;
    ResNode* p = get_resource<ResNode>(&id);
    ASSERT_NE(nullptr, p);
    p->value = 42;
    EXPECT_EQ(p, address_resource<ResNode>(id));
    EXPECT_EQ(42, address_resource<ResNode>(id)->value);
    EXPECT_EQ(0, return_resource<ResNode>(id));
}

TEST(ResourcePoolTest, DistinctIds) {
    ResourceId<ResNode> id1, id2;
    ResNode* p1 = get_resource<ResNode>(&id1);
    ResNode* p2 = get_resource<ResNode>(&id2);
    ASSERT_NE(nullptr, p1);
    ASSERT_NE(nullptr, p2);
    EXPECT_NE(p1, p2);
    EXPECT_NE(id1.value, id2.value);
}

TEST(ResourcePoolTest, Reuse) {
    ResourceId<ResNode> id1, id2;
    ResNode* p1 = get_resource<ResNode>(&id1);
    ASSERT_NE(nullptr, p1);
    EXPECT_EQ(0, return_resource<ResNode>(id1));
    ResNode* p2 = get_resource<ResNode>(&id2);
    ASSERT_NE(nullptr, p2);
    EXPECT_EQ(id1.value, id2.value);  // id is reused
    EXPECT_EQ(p1, p2);
}

// ---- object_pool ----

struct ObjNode {
    ObjNode() : value(0) {}
    int value;
};

TEST(ObjectPoolTest, GetReturn) {
    ObjNode* p = get_object<ObjNode>();
    ASSERT_NE(nullptr, p);
    p->value = 7;
    EXPECT_EQ(0, return_object<ObjNode>(p));
}

TEST(ObjectPoolTest, Reuse) {
    ObjNode* p1 = get_object<ObjNode>();
    ASSERT_NE(nullptr, p1);
    EXPECT_EQ(0, return_object<ObjNode>(p1));
    ObjNode* p2 = get_object<ObjNode>();
    // Single-thread local free list reuses the returned object.
    EXPECT_EQ(p1, p2);
}

}  // namespace
}  // namespace butil
}  // namespace fast
