# Phase 1: OneWayButex

> SQ 满时阻塞等待机制

---

## 目标

实现简化版的阻塞等待机制，用于 KeepWrite 线程在 SQ 满时阻塞，Poller 线程收到 WC 后唤醒。

---

## 使用场景

**一对一场景**：
- **KeepWrite 线程**：SQ 满时 `wait()` 阻塞
- **Poller 线程**：收到 send WC 后 `wake_all()` 唤醒

每个 `RdmaEndpoint` 实例对应一个 `OneWayButex`，`KeepWrite` 线程（当前暂不实现）在无法继续写入时会阻塞在对应Butex上。

---

## 测试目标

- [ ] **基本功能**：wait 阻塞，wake_all 唤醒
- [ ] **一对一**：KeepWrite 线程 wait，Poller 线程 wake
- [ ] **consume**：唤醒后 value 重置为 0

---

## 接口设计

```cpp
namespace fast {

struct OneWayButex {
  OneWayButex() = default;
  ~OneWayButex() = default;

  // 阻塞等待，直到 wake_all 被调用
  void wait();

  // 唤醒所有等待的线程
  void wake_all();

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int value_ = 0;  // 0=idle, 1=wakeup pending
};

} // namespace fast
```

---

## 测试用例

### 1. 基本功能

```cpp
TEST(OneWayButex, Basic) {
    OneWayButex butex;

    // wait 阻塞
    std::thread waiter([&] {
        butex.wait();
    });

    // 短暂等待确保线程已阻塞
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 唤醒
    butex.wake_all();
    waiter.join();
}
```

### 2. 多线程唤醒

```cpp
TEST(OneWayButex, WakeAll) {
    OneWayButex butex;
    std::atomic<int> woke_count{0};

    auto waiter_func = [&] {
        butex.wait();
        woke_count.fetch_add(1);
    };

    std::thread t1(waiter_func);
    std::thread t2(waiter_func);
    std::thread t3(waiter_func);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    butex.wake_all();

    t1.join(); t2.join(); t3.join();
    EXPECT_EQ(woke_count.load(), 3);
}
```

### 3. consume 行为

```cpp
TEST(OneWayButex, Consume) {
    OneWayButex butex;

    std::thread t1([&] { butex.wait(); });
    std::thread t2([&] { butex.wait(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    butex.wake_all();

    t1.join(); t2.join();

    // 再次 wait 应该阻塞
    bool blocked = false;
    std::thread t3([&] {
        butex.wait();
        blocked = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(blocked);

    butex.wake_all();
    t3.join();
}
```

---

## 实现文件

| 文件 | 说明 |
|------|------|
| `inc/one_way_butex.h` | 头文件，包含结构体定义 |
| `src-common/one_way_butex.cc` | 实现 |

---

## 验收标准

1. 测试全部通过
2. 编译无警告
3. 代码符合编码规范

---

## CMakeLists 更新

```cmake
# 公共源文件添加
set(COMMON_SOURCES
    src-common/fast_block_pool.cc
    src-common/fast_define.cc
    src-common/fast_iobuf.cc
    src-common/fast_verbs.cc
    src-common/one_way_butex.cc    # 新增
)

# 单元测试添加
add_executable(unit_tests
    test/unit/test_one_way_butex.cc  # 新增
    ...
)
```

**编译验证**：
```bash
cd build && cmake .. && make
./unit_tests
```

---

## 下一步

Phase 2: EventDispatcher