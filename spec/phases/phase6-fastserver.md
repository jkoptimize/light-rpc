# Phase 7: FastServer

> 服务端端点层集成：集成 EventDispatcher，移除 poller pool

---

## 目标

改造服务端端点层，集成 EventDispatcher，移除 poller pool。

---

## 测试目标

- [ ] **集成 EventDispatcher**：全局 round-robin 分配连接
- [ ] **移除 poller pool**：per-connection CQ 轮询
- [ ] **per-connection CQ 路由**：通过 RdmaEndpoint 处理 WC
- [ ] **Medium 消息分块接收**：累积缓存 IOBuf 判断完整性

---

## 变更点

### 1. 新增成员

```cpp
// fast_server.h
class FastServer {
  // ...
private:
  std::vector<std::unique_ptr<EventDispatcher>> dispatchers_;
  std::atomic<int> dispatcher_idx_;

  // 移除
  // std::vector<std::thread> poller_threads_;
  // SafeHashMap<uint32_t, rdma_cm_id*> conn_id_map_;
};
```

### 2. Init

创建 EventDispatcher 实例（每 CPU 核心一个）。

```cpp
void Init() {
    int num_cores = std::thread::hardware_concurrency();
    for (int i = 0; i < num_cores; ++i) {
        dispatchers_.push_back(std::make_unique<EventDispatcher>(i));
    }
    dispatcher_idx_.store(0);

    // 启动所有 dispatcher
    for (auto& disp : dispatchers_) {
        std::thread t([&] { disp->Run(); });
        t.detach();
    }
}
```

### 3. OnConnect

round-robin 分配到 EventDispatcher。

```cpp
void OnConnect(rdma_cm_id* id) {
    int idx = dispatcher_idx_.fetch_add(1) % dispatchers_.size();
    EventDispatcher* disp = dispatchers_[idx].get();

    // 创建 endpoint
    RdmaEndpoint* ep = shared_rsc_->CreateEndpoint(id->qp, disp);

    // 注册 comp_channel
    int fd = shared_rsc_->GetCompChannelFd(ep);
    disp->AddCompChannel(fd, ep);
}
```

### 4. Medium 消息接收

```cpp
void HandleRecvWC(const ibv_wc& wc) {
    RdmaEndpoint* ep = GetEndpoint(wc.qp_num);

    // cut 到缓存 IOBuf
    ep->AppendRecvData(wc.byte_len, wc.wr_id);

    // 尝试解析完整消息
    while (ep->TryParseMessage()) {
        ProcessMessage(ep->GetParsedMessage());
    }
}
```

---

## 测试用例

### 1. EventDispatcher 初始化

```cpp
TEST(FastServer, DispatcherInit) {
    FastServer server;
    server.Init(8888);

    int num_cores = std::thread::hardware_concurrency();
    EXPECT_EQ(server.GetDispatcherCount(), num_cores);
}
```

### 2. round-robin 分配

```cpp
TEST(FastServer, RoundRobinAllocation) {
    FastServer server;
    server.Init(8888);

    int idx1 = server.AllocateDispatcherIndex();
    int idx2 = server.AllocateDispatcherIndex();
    int idx3 = server.AllocateDispatcherIndex();

    EXPECT_EQ(idx2, (idx1 + 1) % server.GetDispatcherCount());
    EXPECT_EQ(idx3, (idx2 + 1) % server.GetDispatcherCount());
}
```

### 3. Medium 消息接收累积

```cpp
TEST(FastServer, MediumRecvAccumulate) {
    FastServer server;
    server.Init(8888);

    // 模拟接收 3 块，total_len=16KB
    server.SimulateRecvChunk(8 * 1024);  // offset=0
    EXPECT_FALSE(server.MessageComplete());

    server.SimulateRecvChunk(8 * 1024);  // offset=8KB
    EXPECT_TRUE(server.MessageComplete());
}
```

---

## 实现文件

| 文件 | 操作 |
|------|------|
| `inc/fast_server.h` | 修改：添加 dispatchers_、dispatcher_idx_ |
| `src-endpoint/fast_server.cc` | 修改：Init、OnConnect、HandleRecvWC |

---

## 验收标准

1. 测试全部通过
2. 编译无警告
3. 代码符合编码规范

---

## CMakeLists 更新

```cmake
# 单元测试添加
add_executable(unit_tests
    test/unit/test_one_way_butex.cc
    test/unit/test_event_dispatcher.cc
    test/unit/test_rdma_endpoint.cc
    test/unit/test_unique_resource.cc
    test/unit/test_shared_resource.cc
    test/unit/test_fast_channel.cc
    test/unit/test_fast_server.cc       # 新增
)
```

**编译验证**：
```bash
cd build && make
./unit_tests
```

---

## 完成

所有 Phase 完成，Medium 消息分块 + 双窗口流控 + Blocking 功能交付。