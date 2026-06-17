# Phase 5: FastServer

> 服务端端点层改造：集成 EventDispatcher + 回调结构

---

## 目标

1. 每连接创建 FastRdmaEndpoint（per-connection QP/CQ）
2. 回调结构：`OnProcessRequest` — 解析 + dispatch + 序列化响应 + StartWrite
3. EventDispatcher 管理所有连接事件（listen fd + comp_channel fd）
4. 保留 `poller_pool_` / `stop_flag_`（注释标记 deprecated，后续可能复用）

---

## 回调结构

| 回调 | 执行位置 | 职责 |
|------|---------|------|
| `OnProcessRequest` | MessageDispatcher detached thread | 解析帧 → 查 service_map → deserializie → CallMethod → done → ReturnRPCResponse |

`OnSerializeResponse` 不独立存在，序列化逻辑内联在 `ReturnRPCResponse` 中。

---

## 服务端流程

```
连接建立:
  OnServerAccept(EPOLLIN, user_data=FastServer*) → accept fd
    → new FastRdmaEndpoint
    → ep->_owner = server
    → ep->msg_dispatcher().SetMode(kServer)
    → ep->msg_dispatcher().SetHandler(OnProcessRequest, ep)
    → RegisterEvent(client_fd, OnServerHandshake, ..., ep, EPOLLIN|EPOLLET)

握手:
  OnServerHandshake → UnregisterEvent → detach thread:
    ret = ProcessHandshakeAtServer(ep, fd); close(fd)
    if ret == 0:
      ep->_handshake_ok = true
      ep->_owner->AddEndpoint(ep->qp()->qp_num, ep)
    else: delete ep

请求处理:
  PollCq → HandleCompletion(recv WC) → read_buf_
    → MessageDispatcher::ProcessNewMessage
      → CutInputMessage 切帧 → detached thread → OnProcessRequest(frame, arg=ep):
          解析 [total_len][meta_len][meta][payload][attachment]
          server = ep->_owner
          查 server->service_map_
          deserialize request, new response
          done = NewCallback(server, &FastServer::ReturnRPCResponse, args)
          service->CallMethod(request, response, done)

响应返回:
  ReturnRPCResponse(args):
    RAII guard request/response
    构建 IOBuf frame: [total_len][rpc_id][attachment_size][payload][attachment]
    args.endpoint->StartWrite(std::move(frame))
```

---

## 接口设计

```cpp
class FastServer {
public:
  FastServer(std::string local_ip, int local_port);
  ~FastServer();

  void AddService(ServiceOwnership ownership, Service* service);
  void BuildAndStart();
  int listen_fd() const { return listen_fd_; }

  // endpoint 生命周期
  void AddEndpoint(uint32_t qp_num, FastRdmaEndpoint* ep);

  static int OnProcessRequest(IOBuf& frame, void* arg);

private:
  void ReturnRPCResponse(CallBackArgs args);

  std::string local_ip_;
  int local_port_;
  int listen_fd_{-1};

  std::mutex conn_mutex_;
  std::unordered_map<uint32_t, FastRdmaEndpoint*> conn_map_;
  std::unordered_map<std::string, ServiceInfo> service_map_;

  // Deprecated — kept for future use
  // int num_pollers_;
  // std::vector<std::thread> poller_pool_;
  // volatile std::atomic<bool> stop_flag_;
};
```

### CallBackArgs 修改

```cpp
struct CallBackArgs {
  uint32_t rpc_id;
  FastRdmaEndpoint* endpoint;  // was: rdma_cm_id* conn_id
  Message* request;
  Message* response;
  IOBuf request_attachment;
  IOBuf response_attachment;
};
```

### FastRdmaEndpoint 新增

```cpp
class FastRdmaEndpoint {
  // ...
  class FastServer* _owner = nullptr;  // server-side: owning FastServer
};
```

---

## 与旧代码的关键差异

| 维度 | 旧 | 新 |
|------|----|----|
| 连接管理 | librdmacm (`rdma_accept`) + `BuildAndStart` 阻塞循环 | TCP accept + EventDispatcher 驱动 |
| 资源层 | SharedResource (SRQ + 共享 CQ + boost::asio) | FastRdmaEndpoint 自管理 |
| CQ 轮询 | poller_pool_ busy-poll + IBVEventNotifyWait | EventDispatcher + per-connection PollCq |
| WC 路由 | conn_id_map_ (qp_num → rdma_cm_id) | PollCq 在 endpoint 内部闭环，无需路由 |
| IO 分发 | boost::asio::io_context::post | MessageDispatcher detached thread |
| 响应写入 | Inline in ReturnRPCResponse | endpoint->StartWrite → KeepWrite |
| 消息接收 | ProcessRecvWorkCompletion → io_ctx->post | HandleCompletion + read_buf_ + MessageDispatcher |

---

## 已知局限

- 不支持 disconnect 场景，endpoint 仅析构时清理
- Large 消息路径待适配（同 Phase 4）
- 未做 service CallMethod 线程池，直接在 MessageDispatcher detached thread 中执行

---

## 实现文件

| 文件 | 操作 |
|------|------|
| `inc/fast_rdma_endpoint.h` | 修改：添加 `_owner` 成员、OnServerAccept 改造 |
| `src-endpoint/fast_rdma_endpoint.cc` | 修改：OnServerAccept 接收 FastServer*、OnServerHandshake 成功加 AddEndpoint |
| `inc/fast_server.h` | 重写 |
| `src-endpoint/fast_server.cc` | 重写 |
| `inc/fast_define.h` | 修改：CallBackArgs conn_id → endpoint |
| `test/server.cc` | 适配新 FastServer 接口 |

---

## 下一步

Phase 5 完成后所有 Phase 交付。
