# Phase 4: SharedResource

> 服务端资源层改造：移除 SRQ，改为 per-connection QP/CQ

---

## 目标

改造服务端资源层，支持 per-connection QP/CQ，移除共享 SRQ。

---

## 测试目标

- [ ] **移除 SRQ**：CreateNewQueuePair 不再使用 SRQ
- [ ] **per-connection QP/CQ 创建**
- [ ] **移除 conn_id_map_**：per-connection CQ 后不需要 WC 路由查表
- [ ] **注册到 EventDispatcher**

---

## 变更点

### 1. 移除 SRQ

```cpp
// fast_shared.h
class SharedResource {
  // 移除
  // ibv_srq* srq_;

  // 新增：每个连接独立的 QP/CQ
  std::unordered_map<uint32_t, RdmaEndpoint*> endpoint_map_;
};
```

### 2. CreateNewQueuePair

与客户端一致，返回独立的 QP 和 CQ。

```cpp
ibv_qp* CreateNewQueuePair(
    rdma_cm_id* cm_id,
    ibv_cq** send_cq,
    ibv_cq** recv_cq
);
```

### 3. 移除 conn_id_map_

WC 路由直接通过 RdmaEndpoint 处理，无需查表。

```cpp
// 移除
// SafeHashMap<uint32_t, rdma_cm_id*> conn_id_map_;
```

---

## 测试用例

### 1. 无 SRQ

```cpp
TEST(SharedResource, NoSRQ) {
    SharedResource rsc;
    rsc.Init();

    // SRQ 应为 nullptr
    EXPECT_EQ(rsc.GetSRQ(), nullptr);
}
```

### 2. per-connection QP/CQ

```cpp
TEST(SharedResource, PerConnectionQC) {
    SharedResource rsc;
    rsc.Init();

    ibv_cq* send_cq1 = nullptr;
    ibv_cq* recv_cq1 = nullptr;
    ibv_qp* qp1 = rsc.CreateNewQueuePair(nullptr, &send_cq1, &recv_cq1);

    ibv_cq* send_cq2 = nullptr;
    ibv_cq* recv_cq2 = nullptr;
    ibv_qp* qp2 = rsc.CreateNewQueuePair(nullptr, &send_cq2, &recv_cq2);

    EXPECT_NE(send_cq1, send_cq2);
    EXPECT_NE(recv_cq1, recv_cq2);
    EXPECT_NE(qp1, qp2);
}
```

### 3. 注册 endpoint

```cpp
TEST(SharedResource, RegisterEndpoint) {
    SharedResource rsc;
    rsc.Init();

    EventDispatcher disp(0);
    std::thread t([&] { disp.Run(); });

    ibv_cq* send_cq = nullptr;
    ibv_cq* recv_cq = nullptr;
    ibv_qp* qp = rsc.CreateNewQueuePair(nullptr, &send_cq, &recv_cq);

    RdmaEndpoint* ep = rsc.CreateEndpoint(qp, &disp);

    EXPECT_NE(ep, nullptr);

    disp.Stop();
    t.join();
}
```

---

## 实现文件

| 文件 | 操作 |
|------|------|
| `inc/fast_shared.h` | 修改：移除 SRQ，添加 endpoint_map_ |
| `src-resource/fast_shared.cc` | 修改：CreateNewQueuePair、CreateEndpoint |

---

## CMakeLists 更新

新增测试：`test/unit/test_shared_resource.cc`

---

## 下一步

Phase 5: FastChannel