# Phase 3: UniqueResource

> 客户端资源层改造：send/recv CQ 共用一份 comp_channel

---

## 目标

改造客户端资源层，支持 per-connection QP/CQ，send/recv CQ 共用 comp_channel。

---

## 测试目标

- [ ] **send/recv CQ 共用 comp_channel**
- [ ] **per-connection QP/CQ 创建**
- [ ] **注册到 EventDispatcher**

---

## 变更点

### 1. comp_channel 共享

原设计：send_cq、recv_cq 各一份 comp_channel

新设计：send_cq、recv_cq 共用一份 comp_channel

```cpp
// fast_unique.h
class UniqueResource {
  // ...
private:
  ibv_comp_channel* comp_channel_;  // 共用
  // 移除 recv_comp_channel_
};
```

### 2. CreateNewQueuePair

返回 QP 和独立的 send_cq、recv_cq，comp_channel 共享。

```cpp
ibv_qp* CreateNewQueuePair(
    rdma_cm_id* cm_id,
    ibv_cq** send_cq,
    ibv_cq** recv_cq
);
```

### 3. RdmaEndpoint 创建

CreateConnection 时创建 RdmaEndpoint 实例。

```cpp
RdmaEndpoint* CreateEndpoint(ibv_qp* qp, EventDispatcher* disp);
```

---

## 测试用例

### 1. CQ 共用 comp_channel

```cpp
TEST(UniqueResource, SharedCompChannel) {
    UniqueResource rsc;
    rsc.Init();

    ibv_cq* send_cq = nullptr;
    ibv_cq* recv_cq = nullptr;

    ibv_qp* qp = rsc.CreateNewQueuePair(nullptr, &send_cq, &recv_cq);

    EXPECT_NE(send_cq, nullptr);
    EXPECT_NE(recv_cq, nullptr);
    EXPECT_NE(qp, nullptr);

    // 验证 comp_channel 相同
    EXPECT_EQ(send_cq->channel, recv_cq->channel);
}
```

### 2. 注册到 EventDispatcher

```cpp
TEST(UniqueResource, RegisterToDispatcher) {
    UniqueResource rsc;
    rsc.Init();

    EventDispatcher disp(0);
    std::thread t([&] { disp.Run(); });

    // 创建 QP
    ibv_cq* send_cq = nullptr;
    ibv_cq* recv_cq = nullptr;
    ibv_qp* qp = rsc.CreateNewQueuePair(nullptr, &send_cq, &recv_cq);

    // 创建 endpoint
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
| `inc/fast_unique.h` | 修改：移除 recv_comp_channel_ |
| `src-resource/fast_unique.cc` | 修改：CreateNewQueuePair、CreateEndpoint |

---

## CMakeLists 更新

新增测试：`test/unit/test_unique_resource.cc`

---

## 下一步

Phase 4: SharedResource