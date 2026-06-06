# Phase 5: FastServer

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

1. **新增成员**：`fast_server.h` 添加 `dispatchers_`（EventDispatcher 数组）、`dispatcher_idx_`（round-robin 索引），移除 `poller_threads_`、`conn_id_map_`
2. **Init**：创建 EventDispatcher 实例（每 CPU 核心一个）并启动
3. **OnConnect**：round-robin 分配到 EventDispatcher，创建 RdmaEndpoint 并注册 comp_channel fd
4. **Medium 消息接收**：通过 RdmaEndpoint 累积 recv buffer，判断消息完整性后处理

> 具体实现细节较多，暂不在此文档中展开，实现时参考 brpc `rdma_endpoint.cpp` 中 `HandleCompletion` / `PostRecv` 的模式。

---

## 实现文件

| 文件 | 操作 |
|------|------|
| `inc/fast_server.h` | 修改：添加 dispatchers_、dispatcher_idx_ |
| `src-endpoint/fast_server.cc` | 修改：Init、OnConnect、HandleRecvWC |

---

## CMakeLists 更新

新增测试：`test/unit/test_fast_server.cc`

---

## 完成

所有 Phase 完成，Medium 消息分块 + 双窗口流控 + Blocking 功能交付。