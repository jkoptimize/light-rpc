# Phase 4: FastChannel

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

核心思路：将 IOBuf 的 block 按序切入 `ibv_sge` 数组，每批不超过 `MAX_SGE` 个 entry、不超过 `block_size` 字节，切成 SGE 后通过 `EnqueueSend` 发出。被切出的数据移动到 `_sbuf` 中管理生命周期，等 send WC 返回后释放。

参考 brpc `rdma_endpoint.cpp` 中 `cut_into_sglist_and_iobuf` + `CutFromIOBufList` 的逻辑：

```cpp
// 从 IOBuf 中切出数据填入 sglist，切出的数据移动到 holder 管理生命周期
// 返回填写的 sge 数量，或 -1 表示失败
ssize_t CutIOBufIntoSGE(IOBuf& src, ibv_sge* sglist, size_t max_sge,
                         uint32_t block_size, IOBuf* holder) {
    size_t sge_count = 0;
    size_t total = 0;
    while (sge_count < max_sge && !src.empty() && total < block_size) {
        const IOBuf::BlockRef& ref = src.front_ref();
        void* addr = src.fetch1();
        uint32_t lkey = GetBlockLKey(ref, addr);
        if (lkey == 0) {
            errno = ERDMAMEM;
            return -1;
        }
        size_t len = std::min(ref.length, block_size - total);
        sglist[sge_count++] = { (uint64_t)addr, len, lkey };
        src.cutn(holder, len);
        total += len;
    }
    return sge_count;
}

void FastChannel::SendMediumMessage(IOBuf& frame) {
    ibv_sge sglist[MAX_SGE];
    while (!frame.empty()) {
        ssize_t n = CutIOBufIntoSGE(frame, sglist, MAX_SGE,
                                     block_size_, &_sbuf[_sq_current]);
        if (n > 0) {
            endpoint_->EnqueueSend(sglist, n, 0);
        }
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

    // 构造 16KB 测试数据（含 2 个 8KB block）
    char test_data[16 * 1024];
    memset(test_data, 'A', sizeof(test_data));
    IOBuf frame;
    frame.append(test_data, sizeof(test_data));
    ASSERT_EQ(frame.size(), 16384);

    channel.SendMediumMessage(frame);

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

## CMakeLists 更新

新增测试：`test/unit/test_fast_channel.cc`

---

## 下一步

Phase 5: FastServer