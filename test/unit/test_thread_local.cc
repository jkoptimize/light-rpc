#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

#include "butil/thread_local.h"
#include "butil/thread_key.h"

namespace fast {
namespace butil {
namespace {

static std::atomic<int> g_atexit_count{0};
static std::vector<int> g_order;

static void atexit_inc(void* arg) {
    (void)arg;
    g_atexit_count.fetch_add(1, std::memory_order_relaxed);
}

static void atexit_push_1(void* arg) {
    (void)arg;
    g_order.push_back(1);
}

static void atexit_push_2(void* arg) {
    (void)arg;
    g_order.push_back(2);
}

// thread_atexit 在线程退出时调用，LIFO 顺序。
TEST(ThreadLocalTest, ThreadAtexitLifo) {
    g_order.clear();
    std::thread t([]() {
        thread_atexit(atexit_push_1, nullptr);
        thread_atexit(atexit_push_2, nullptr);
    });
    t.join();
    ASSERT_EQ(2u, g_order.size());
    EXPECT_EQ(2, g_order[0]);  // 后注册的先调用
    EXPECT_EQ(1, g_order[1]);
}

// thread_atexit_cancel 取消已注册的回调。
TEST(ThreadLocalTest, ThreadAtexitCancel) {
    g_atexit_count.store(0);
    std::thread t([]() {
        thread_atexit(atexit_inc, nullptr);
        thread_atexit_cancel(atexit_inc, nullptr);
    });
    t.join();
    EXPECT_EQ(0, g_atexit_count.load());
}

// ThreadLocal<T> 在每个线程返回独立对象，主线程对象在 join 后仍有效。
TEST(ThreadLocalTest, ThreadLocalPerThread) {
    ThreadLocal<int> tl;
    int* main_ptr = tl.get();
    *main_ptr = 1;

    std::thread t([&tl, main_ptr]() {
        int* ptr = tl.get();
        EXPECT_NE(ptr, main_ptr);
        *ptr = 2;
    });
    t.join();
    EXPECT_EQ(1, *main_ptr);
}

}  // namespace
}  // namespace butil
}  // namespace fast
