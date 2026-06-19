#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "fast_utils.h"

using namespace fast;

// ============================================================
// SafeHashMap
// ============================================================

TEST(SafeHashMap, InsertAndGet) {
    SafeHashMap<int*> map;
    int val = 42;
    map.SafeInsert(1, &val);
    EXPECT_EQ(map.SafeGet(1), &val);
}

TEST(SafeHashMap, InsertExistingKeyNoOverwrite) {
    // std::unordered_map::emplace does NOT overwrite existing keys
    SafeHashMap<int*> map;
    int a = 1, b = 2;
    map.SafeInsert(1, &a);
    map.SafeInsert(1, &b);   // no-op: key 1 already exists
    EXPECT_EQ(map.SafeGet(1), &a);
}

TEST(SafeHashMap, GetNonExistentKey) {
    SafeHashMap<int*> map;
    EXPECT_DEATH(map.SafeGet(42), "");
}

TEST(SafeHashMap, Erase) {
    SafeHashMap<int*> map;
    int val = 7;
    map.SafeInsert(1, &val);
    map.SafeErase(1);
    EXPECT_DEATH(map.SafeGet(1), "");
}

TEST(SafeHashMap, EraseNonExistentKey) {
    SafeHashMap<int*> map;
    EXPECT_DEATH(map.SafeErase(42), "");
}

TEST(SafeHashMap, GetAndErase) {
    SafeHashMap<int*> map;
    int val = 99;
    map.SafeInsert(1, &val);
    int* ret = map.SafeGetAndErase(1);
    EXPECT_EQ(ret, &val);
    EXPECT_DEATH(map.SafeGet(1), "");
}

TEST(SafeHashMap, ConcurrentInsertAndGet) {
    SafeHashMap<int> map;
    const int kThreads = 4;
    const int kPerThread = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&map, t] {
            for (int i = 0; i < kPerThread; ++i) {
                uint32_t key = static_cast<uint32_t>(t * kPerThread + i);
                map.SafeInsert(key, key);
            }
        });
    }
    for (auto& th : threads) th.join();

    // Verify all inserts
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            uint32_t key = static_cast<uint32_t>(t * kPerThread + i);
            EXPECT_EQ(map.SafeGet(key), key);
        }
    }
}

// ============================================================
// ScopeGuard
// ============================================================

TEST(ScopeGuard, ExecuteOnScopeExit) {
    bool called = false;
    {
        auto guard = MakeScopeGuard([&called] { called = true; });
        EXPECT_FALSE(called);
    }
    EXPECT_TRUE(called);
}

TEST(ScopeGuard, DismissPreventsExecution) {
    bool called = false;
    {
        auto guard = MakeScopeGuard([&called] { called = true; });
        guard.dismiss();
    }
    EXPECT_FALSE(called);
}

TEST(ScopeGuard, MoveConstructorTransfersOwnership) {
    bool called = false;
    {
        auto guard1 = MakeScopeGuard([&called] { called = true; });
        {
            auto guard2 = std::move(guard1);
            EXPECT_FALSE(called);
        }
        // guard2 destructor fires the callback
        EXPECT_TRUE(called);
        called = false;
    }
    // guard1 was dismissed by move, should not fire again
    EXPECT_FALSE(called);
}

TEST(ScopeGuard, MoveConstructorDismissesSource) {
    bool called = false;
    auto guard1 = MakeScopeGuard([&called] { called = true; });
    auto guard2 = std::move(guard1);
    // guard1 is now dismissed
    // guard2 holds the callback
    EXPECT_FALSE(called);
}

// ============================================================
// ThreadExitHelper
// ============================================================

TEST(ThreadExitHelper, CallbackOnThreadExit) {
    std::atomic<bool> called{false};

    std::thread([&called] {
        ThreadExitHelper::add_callback([&called] { called.store(true); });
    }).join();

    EXPECT_TRUE(called.load());
}

TEST(ThreadExitHelper, MultipleCallbacksInReverseOrder) {
    std::atomic<int> counter{0};
    std::atomic<int> first{0};
    std::atomic<int> second{0};

    std::thread([&] {
        ThreadExitHelper::add_callback([&] {
            first.store(++counter);
        });
        ThreadExitHelper::add_callback([&] {
            second.store(++counter);
        });
        // second was added after first, so second fires first (reverse)
    }).join();

    // Reverse order: second fires first → counter=1, first fires second → counter=2
    EXPECT_EQ(second.load(), 1);
    EXPECT_EQ(first.load(), 2);
}

TEST(ThreadExitHelper, ThreadLocalIsolation) {
    std::atomic<int> call_count{0};

    auto thread_func = [&call_count] {
        ThreadExitHelper::add_callback([&call_count] {
            call_count.fetch_add(1);
        });
    };

    std::thread t1(thread_func);
    std::thread t2(thread_func);
    t1.join();
    t2.join();

    EXPECT_EQ(call_count.load(), 2);
}
