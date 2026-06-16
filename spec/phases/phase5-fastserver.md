# Phase 5: FastServer

> 服务端端点层改造：集成 EventDispatcher + 回调结构，移除 poller pool 和 SharedResource

---

## 目标

1. 移除 SharedResource（librdmacm SRQ）、poller_pool_、conn_id_map_、boost::asio::io_context
2. 每连接创建 FastRdmaEndpoint（per-connection QP/CQ）
3. 回调结构：`OnProcessRequest` / `OnSerializeResponse`
4. EventDispatcher 管理所有连接事件

---

## 回调结构

| 回调 | 执行位置 | 职责 |
|------|---------|------|
| `OnProcessRequest` | MessageDispatcher detached thread | 解析帧 → 查 service_map → deserialize → `service->CallMethod(request, response, done)` |
| `OnSerializeResponse` | service done 回调线程 | `args.endpoint->StartWrite(response_frame)` |

---

## 服务端流程

```
连接建立:
  OnServerAccept (EPOLLIN) → accept fd
    → new FastRdmaEndpoint → OnServerHandshake
    → ProcessHandshakeAtServer (TCP HelloMessage + QP)
    → AllocateResources (自动注册 comp_channel 到 EventDispatcher)
    → 设置 MessageDispatcher handler = OnProcessRequest
    → PollCq 启动

请求处理:
  PollCq → HandleCompletion(recv WC) → read_buf_
    → MessageDispatcher::ProcessNewMessage
      → CutInputMessage 切帧
      → detached thread → OnProcessRequest:
          解析 [total_len][meta_len][meta][payload][attachment]
          根据 service_name/method_name 查找 service
          deserialize request
          service->CallMethod(request, response, done=ReturnRPCResponse)

响应返回:
  ReturnRPCResponse(args):
    构建 frame: [total_len][rpc_id][attachment_size][payload][attachment]
    args.endpoint->StartWrite(std::move(frame))
```

---

## 接口设计

```cpp
class FastServer {
public:
  FastServer(std::string local_ip, int local_port);
  ~FastServer();

  void AddService(ServiceOwnership ownership, google::protobuf::Service* service);
  void BuildAndStart();

private:
  // 回调
  int OnProcessRequest(IOBuf& frame, void* arg);
  void OnSerializeResponse(CallBackArgs args);

  // 每连接端点管理
  struct ConnContext {
    FastRdmaEndpoint* endpoint;
  };
  std::unordered_map<uint32_t, ConnContext> conn_map_;  // qp_num → context

  std::string local_ip_;
  int local_port_;
  std::unordered_map<std::string, ServiceInfo> service_map_;

  int listen_fd_{-1};  // TCP listen fd，注册到 EventDispatcher
};
```

---

## 与旧代码的关键差异

| 维度 | 旧 | 新 |
|------|----|----|
| 连接管理 | librdmacm (`rdma_accept`) | TCP accept + raw ibv_verbs |
| 资源层 | SharedResource (SRQ + 共享 CQ) | 无资源层，FastRdmaEndpoint 自管理 |
| CQ 轮询 | poller_pool_ busy-poll + IBVEventNotifyWait | EventDispatcher + per-connection PollCq |
| WC 路由 | conn_id_map_ (qp_num → rdma_cm_id) | 无需路由，PollCq 在 endpoint 内部闭环 |
| IO 分发 | boost::asio::io_context::post | MessageDispatcher detached thread |
| 响应写入 | Inline in ReturnRPCResponse | endpoint_->StartWrite → KeepWrite |
| 大消息 | LargeBlockAlloc (SharedResource) | LargeBlockAlloc (每种资源各自) |
| 消息接收 | ProcessRecvWorkCompletion → io_ctx->post | HandleCompletion + read_buf_ + MessageDispatcher |

---

## 实现文件

| 文件 | 操作 |
|------|------|
| `inc/fast_server.h` | 重写 |
| `src-endpoint/fast_server.cc` | 重写 |

---

## CMakeLists 更新

新增测试：`test/unit/test_fast_server.cc`

---

## 完成

所有 Phase 完成，Medium 消息分块 + 双窗口流控 + Blocking 功能交付。
