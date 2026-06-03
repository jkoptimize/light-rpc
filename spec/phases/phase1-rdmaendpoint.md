# Phase 3: RdmaEndpoint

> per-connection 抽象，封装连接语义

---

## 目标

实现 per-connection 的封装类，管理流控、发送队列、CQ 轮询。

---

## 测试目标

- [ ] **流控变量初始化**：sq_window_size、remote_rq_window_size 等
- [ ] **PollCq**：处理 send/recv WC
- [ ] **KeepWrite**：SQ 满时阻塞在 OneWayButex
- [ ] **ProcessWC**：recv WC 累积到缓存 IOBuf，判断完整性
- [ ] **wr_id 编码**：普通 SEND vs Pure ACK

---

## 接口设计

```cpp
namespace fast {

class RdmaEndpoint {
public:
  explicit RdmaEndpoint(ibv_qp* qp, EventDispatcher* disp);
  ~RdmaEndpoint();

  // CQ 轮询线程入口
  void PollCq();

  // 后台发送线程入口
  void KeepWrite();

  // WC 处理
  void ProcessWC(const ibv_wc& wc);

  // EnqueueSend：io_context 线程调用，原子入队
  void EnqueueSend(ibv_sge* sges, int sge_count, uint32_t imm_data);

  // 窗口访问
  int sq_window_size() const { return sq_window_size_; }
  int remote_rq_window_size() const { return remote_rq_window_size_; }

private:
  ibv_qp* qp_;
  EventDispatcher* dispatcher_;

  // 流控变量
  std::atomic<int> sq_window_size_;
  std::atomic<int> remote_rq_window_size_;
  std::atomic<int> _sq_imm_window_size_;
  std::atomic<int> _new_rq_wrs_;

  // 发送队列
  struct SendRequest {
    ibv_sge* sges;
    int sge_count;
    uint32_t imm_data;
  };
  std::atomic<SendRequest*> pending_head_;
  std::atomic<SendRequest*> pending_tail_;

  // 阻塞等待
  OneWayButex butex_;

  // 接收缓存
  IOBuf recv_buf_;

  // selective signaling
  int send_counter_;
};

} // namespace fast
```

---

## 测试用例

### 1. 流控变量初始化

```cpp
TEST(RdmaEndpoint, FlowControlInit) {
    RdmaEndpoint ep(nullptr, nullptr);
    EXPECT_EQ(ep.sq_window_size(), 29);  // 32 - 3
    EXPECT_EQ(ep.remote_rq_window_size(), 29);
}
```

### 2. 窗口更新

```cpp
TEST(RdmaEndpoint, WindowUpdate) {
    RdmaEndpoint ep(nullptr, nullptr);

    // 模拟 send WC
    ep.HandleSendWC();
    EXPECT_EQ(ep.sq_window_size(), 30);

    // 模拟 recv WC（imm_data=1）
    ep.HandleRecvWC(1);
    EXPECT_EQ(ep.remote_rq_window_size(), 30);
}
```

### 3. Pure ACK 触发条件

```cpp
TEST(RdmaEndpoint, PureAckTrigger) {
    RdmaEndpoint ep(nullptr, nullptr);

    // 不触发：_new_rq_wrs 不足
    ep.SetNewRqWrs(10);
    EXPECT_FALSE(ep.ShouldSendPureAck());

    // 触发：_new_rq_wrs > 29/2 = 14
    ep.SetNewRqWrs(15);
    EXPECT_TRUE(ep.ShouldSendPureAck());
}
```

### 4. EnqueueSend + KeepWrite

```cpp
TEST(RdmaEndpoint, EnqueueSendAndKeepWrite) {
    RdmaEndpoint ep(nullptr, nullptr);

    ibv_sge sges[1];
    sges[0].addr = 0x1000;
    sges[0].length = 1024;
    sges[0].lkey = 0;

    ep.EnqueueSend(sges, 1, 0);

    // 启动 KeepWrite 线程
    std::thread t([&] { ep.KeepWrite(); });

    // 等待处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 停止
    ep.Stop();
    t.join();

    // 验证请求被处理
    EXPECT_TRUE(ep.PendingEmpty());
}
```

---

## 实现文件

| 文件 | 说明 |
|------|------|
| `inc/rdma_endpoint.h` | 头文件 |
| `src-endpoint/rdma_endpoint.cc` | 实现 |

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
    src-endpoint/event_dispatcher.cc
    src-endpoint/rdma_endpoint.cc       # 新增
)

# 单元测试添加
add_executable(unit_tests
    test/unit/test_one_way_butex.cc
    test/unit/test_event_dispatcher.cc
    test/unit/test_rdma_endpoint.cc     # 新增
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

Phase 4: UniqueResource