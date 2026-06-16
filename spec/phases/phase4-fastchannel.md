# Phase 4: FastChannel

> 客户端端点层改造：集成 FastRdmaEndpoint + 回调结构 + WriteQueue

---

## 目标

1. FastChannel 持有 FastRdmaEndpoint，通过 TCP 握手建立 RDMA 连接
2. 写入路径：序列化 → `endpoint_->StartWrite()`（内部 KeepWrite 异步发送）
3. 响应路径：MessageDispatcher (client mode) → `OnProcessResponse` 回调
4. 同步调用：CallMethod 阻塞等待响应（condition_variable）

---

## 回调结构

| 回调 | 执行位置 | 职责 |
|------|---------|------|
| `OnSerializeRequest` | CallMethod 调用线程 | protobuf 序列化 → IOBuf frame → `endpoint_->StartWrite()` |
| `OnProcessResponse` | MessageDispatcher detached thread | 解析 `[total_len][rpc_id][attachment_size][payload][attachment]` → 查 pending → 填充 response → cv.notify() |

---

## 调用流程

```
CallMethod(request, response):
  1. OnSerializeRequest:
     - 构建 frame: [total_len][meta_len][meta][payload][attachment]
     - rpc_id = rpc_id_++
     - 记录 pending: rpc_id → {cv, response*}
     - endpoint_->StartWrite(std::move(frame))
  2. cv.wait() // 阻塞等待响应
  
  // 响应由 OnProcessResponse 唤醒
```

---

## 响应分发

```
PollCq → HandleCompletion(recv WC) → read_buf_
  → MessageDispatcher::ProcessNewMessage
    → CutInputMessage 切帧 (total_len at offset 0)
    → detached thread → OnProcessResponse:
        解析 [total_len][rpc_id][attachment_size][payload][attachment]
        查 pending map → 填充 response → cv.notify()
```

---

## 连接建立

复用已有 `FastRdmaEndpoint` 的 client 握手回调链：

```
TCP connect → OnClientHandshake (EPOLLOUT)
  → ProcessHandshakeAtClient (TCP HelloMessage 交换 + QP setup)
  → AllocateResources (自动注册 comp_channel 到 EventDispatcher)
  → PollCq 启动
```

---

## 关键接口变更

```cpp
class FastChannel : public google::protobuf::RpcChannel {
public:
  FastChannel(std::string dest_ip, int dest_port);
  ~FastChannel();

  void CallMethod(...) override;  // 重写

  // 附件接口保持
  void SetRequestAttachment(IOBuf&& attachment);
  void SetRequestAttachment(const void* data, size_t len);
  const IOBuf& ResponseAttachment() const;

private:
  // 回调
  IOBuf OnSerializeRequest(const google::protobuf::MethodDescriptor* method,
                           const google::protobuf::Message* request);
  int OnProcessResponse(IOBuf& frame, void* arg);

  FastRdmaEndpoint* endpoint_;

  uint32_t rpc_id_{1};
  IOBuf request_attachment_;
  IOBuf response_attachment_;

  // Pending requests
  struct PendingRequest {
    std::condition_variable cv;
    google::protobuf::Message* response;
    bool done = false;
  };
  std::mutex pending_mutex_;
  std::unordered_map<uint32_t, PendingRequest*> pending_;
};
```

---

## Proto 修改

```proto
message ResponseHead {
  fixed32 total_len = 1;       // 调到第一位，MessageDispatcher 统一 offset=0
  fixed32 rpc_id = 2;
  uint32 attachment_size = 3;
}
```

---

## 与旧代码的关键差异

| 维度 | 旧 | 新 |
|------|----|----|
| 连接建立 | librdmacm (`rdma_connect`) | TCP + raw ibv_verbs 握手 |
| 资源管理 | UniqueResource (librdmacm QP) | FastRdmaEndpoint (per-connection QP/CQ) |
| 写入 | Inline in CallMethod (SendInlineMessage/SendMiddleMessage/...) | endpoint_->StartWrite → KeepWrite |
| 响应 | busy-poll recv CQ in CallMethod | PollCq + MessageDispatcher 异步分发 |
| CQ 轮询 | 每次 CallMethod 手动 poll | EventDispatcher + PollCq 持续运行 |

---

## 实现文件

| 文件 | 操作 |
|------|------|
| `proto/fast_impl.proto` | 修改：ResponseHead total_len 调为 field 1 |
| `inc/fast_rdma_endpoint.h` | 修改：添加 WriteRequest, StartWrite, KeepWrite, IsWriteComplete |
| `src-endpoint/fast_rdma_endpoint.cc` | 修改：实现 WriteQueue + 完善 MessageDispatcher mode |
| `inc/message_dispatcher.h` | 修改：添加 DispatcherMode |
| `src-endpoint/message_dispatcher.cc` | 修改：mode 支持 |
| `inc/fast_channel.h` | 重写 |
| `src-endpoint/fast_channel.cc` | 重写 |

---

## CMakeLists 更新

新增测试：`test/unit/test_fast_channel.cc`

---

## 下一步

Phase 5: FastServer
