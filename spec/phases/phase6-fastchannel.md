# Phase 6: FastChannel

> 客户端端点层集成：集成 RdmaEndpoint + EventDispatcher

---

## 目标

改造客户端端点层，集成 RdmaEndpoint 和 EventDispatcher。

---

## 测试目标

- [ ] **集成 RdmaEndpoint**：FastChannel 持有 RdmaEndpoint 实例
- [ ] **集成 EventDispatcher**：注册 comp_channel fd
- [ ] **Medium 消息分块发送**：超过 8KB 的消息分块发送

---

## 变更点

### 1. 新增成员

```cpp
// fast_channel.h
class FastChannel {
  // ...
private:
  RdmaEndpoint* endpoint_;
  EventDispatcher* dispatcher_;
};
```

### 2. CreateConnection

创建 RdmaEndpoint 并注册到 EventDispatcher。

```cpp
void CreateConnection(const std::string& addr, int port) {
    // ... 原有逻辑 ...

    // 创建 endpoint
    endpoint_ = unique_rsc_->CreateEndpoint(qp, dispatcher_);

    // 注册 comp_channel
    int fd = unique_rsc_->GetCompChannelFd();
    dispatcher_->AddCompChannel(fd, endpoint_);
}
```

### 3. Medium 消息分块

将消息按 8KB 分块发送。

```cpp
void SendMediumMessage(IOBuf& frame) {
    const uint32_t chunk_size = g_block_size;  // 8KB

    for (size_t offset = 0; offset < frame.size(); offset += chunk_size) {
        ibv_sge sge;
        sge.addr = GetChunkAddr(frame, offset);
        sge.length = std::min(chunk_size, frame.size() - offset);
        sge.lkey = GetChunkLkey(frame, offset);

        endpoint_->EnqueueSend(&sge, 1, 0);
    }
}
```

---

## 测试用例

### 1. 集成 RdmaEndpoint

```cpp
TEST(FastChannel, IntegratedEndpoint) {
    FastChannel channel;

    channel.Init();
    channel.Connect("127.0.0.1", 8888);

    RdmaEndpoint* ep = channel.GetEndpoint();
    EXPECT_NE(ep, nullptr);
}
```

### 2. 注册到 EventDispatcher

```cpp
TEST(FastChannel, RegisteredToDispatcher) {
    FastChannel channel;

    channel.Init();
    channel.Connect("127.0.0.1", 8888);

    EventDispatcher* disp = channel.GetDispatcher();
    EXPECT_NE(disp, nullptr);

    // 验证 comp_channel fd 已注册
    int fd = channel.GetCompChannelFd();
    EXPECT_GT(fd, 0);
}
```

### 3. Medium 消息分块

```cpp
TEST(FastChannel, MediumMultipartSend) {
    FastChannel channel;

    channel.Init();
    channel.Connect("127.0.0.1", 8888);

    // 创建 16KB 消息（需要分 2 块）
    IOBuf frame;
    frame.AppendRandom(16 * 1024);

    channel.SendMediumMessage(frame);

    // 验证分块数量
    EXPECT_EQ(channel.GetLastChunkCount(), 2);
}
```

---

## 实现文件

| 文件 | 操作 |
|------|------|
| `inc/fast_channel.h` | 修改：添加 endpoint_、dispatcher_ |
| `src-endpoint/fast_channel.cc` | 修改：CreateConnection、SendMediumMessage |

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
    test/unit/test_fast_channel.cc      # 新增
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

Phase 7: FastServer