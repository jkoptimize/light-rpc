# Phase 2: EventDispatcher

> 全局 epoll 事件分发器，每 CPU 核心一个

---

## 目标

实现基于 epoll 的事件分发器，用于管理 listen fd 和 comp_channel fd。

---

## 测试目标

- [ ] **AddListenFd**：fd 可读时触发回调
- [ ] **AddCompChannel**：fd 可读时创建 poller 线程
- [ ] **round-robin**：多 dispatcher 轮询分配
- [ ] **RemoveCompChannel**：注销 comp_channel fd

---

## 接口设计

```cpp
namespace fast {

using EpollCallback = std::function<void()>;

class EventDispatcher {
public:
  explicit EventDispatcher(int index);
  ~EventDispatcher();

  // 注册 listen fd，触发时执行 callback
  void AddListenFd(int fd, EpollCallback callback);

  // 注册 comp_channel fd，触发时执行 RdmaEndpoint::PollCq
  void AddCompChannel(int fd, class RdmaEndpoint* endpoint);

  // 注销 comp_channel fd
  void RemoveCompChannel(int fd);

  // 启动 epoll 循环
  void Run();

  // 停止 epoll 循环
  void Stop();

  int GetIndex() const { return index_; }

private:
  void EpollLoop();

  int epoll_fd_;
  int index_;
  bool running_;
  std::unordered_map<int, EpollCallback> callbacks_;
  std::thread thread_;
};

// 全局 dispatcher 索引，round-robin 分配
extern std::atomic<int> g_dispatcher_idx;

} // namespace fast
```

---

## 测试用例

### 1. AddListenFd 回调

```cpp
TEST(EventDispatcher, ListenFdCallback) {
    EventDispatcher disp(0);

    int pipefd[2];
    pipe(pipefd);

    bool called = false;
    disp.AddListenFd(pipefd[0], [&] { called = true; });

    std::thread t([&] { disp.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    write(pipefd[1], "x", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    disp.Stop();
    t.join();

    EXPECT_TRUE(called);
    close(pipefd[0]); close(pipefd[1]);
}
```

### 2. round-robin 分配

```cpp
TEST(EventDispatcher, RoundRobin) {
    std::vector<std::unique_ptr<EventDispatcher>> dispatchers;
    for (int i = 0; i < 4; ++i) {
        dispatchers.push_back(std::make_unique<EventDispatcher>(i));
    }

    g_dispatcher_idx.store(0);

    int idx1 = g_dispatcher_idx.fetch_add(1) % 4;
    int idx2 = g_dispatcher_idx.fetch_add(1) % 4;
    int idx3 = g_dispatcher_idx.fetch_add(1) % 4;

    EXPECT_EQ(idx1, 0);
    EXPECT_EQ(idx2, 1);
    EXPECT_EQ(idx3, 2);
}
```

### 3. RemoveCompChannel

```cpp
TEST(EventDispatcher, RemoveCompChannel) {
    EventDispatcher disp(0);

    int pipefd[2];
    pipe(pipefd);

    bool called = false;
    disp.AddCompChannel(pipefd[0], nullptr);  // endpoint 传 nullptr
    disp.RemoveCompChannel(pipefd[0]);

    // 不应触发回调
    write(pipefd[1], "x", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_FALSE(called);
    close(pipefd[0]); close(pipefd[1]);
}
```

---

## 实现文件

| 文件 | 说明 |
|------|------|
| `inc/event_dispatcher.h` | 头文件 |
| `src-endpoint/event_dispatcher.cc` | 实现 |
| `src-common/fast_define.cc` | 添加 `g_dispatcher_idx` 初始化 |

---

## 验收标准

1. 测试全部通过
2. 编译无警告
3. 代码符合编码规范

---

## CMakeLists 更新

```cmake
# 端点层源文件添加
set(ENDPOINT_SOURCES
    src-endpoint/fast_channel.cc
    src-endpoint/fast_server.cc
    src-endpoint/event_dispatcher.cc    # 新增
    ...
)

# 单元测试添加
add_executable(unit_tests
    test/unit/test_one_way_butex.cc
    test/unit/test_event_dispatcher.cc  # 新增
    ...
)
```

**编译验证**：
```bash
cd build && make
./unit_tests
```

---

## 下一步

Phase 3: RdmaEndpoint