# Spec: Medium 消息分块 + 双窗口流控 + Blocking

> 更新：2026-05-24
> 模式：TDD (Test-Driven Development)

---

## 一、整体设计大纲

### 1.1 目标

解决 Medium 消息（200B ~ 2MB）的两个问题：
1. recv buffer 仅 8KB（max_sge=1），超过 8KB 的 Medium 消息无法一次 SEND 装载
2. IOBuf block 数量超过 MAX_SGE=32 时 SGE 越界

### 1.2 核心原则

| 原则 | 说明 |
|------|------|
| Medium 路径 | RDMA SEND/RECV 双边（非两阶段 RDMA WRITE） |
| 无新消息类型 | 现有帧头 `[total_len][meta_len]` 足够 |
| per-connection 顺序性 | RC 模式保序，无需追踪 rpc_id |
| per-connection 封装 | 通过 `FastRdmaEndpoint` 类管理 |
| 固定配置 | sq_size = rq_size = 32，block_size = 8KB |
| 非阻塞 EnqueueSend | io_context 线程入队即返 |
| SQ 满时阻塞 | condition_variable（std::condition_variable） |
| TDD | 每个 Phase 先写测试，后实现 |

### 1.3 架构变更

| 组件 | 原设计 | 新设计 |
|------|--------|--------|
| QP RQ | 共享 SRQ | per-connection 独立 |
| recv_cq | 共享 | per-connection 独立 |
| Poller | 线程池 | EventDispatcher epoll（每 CPU 核心一个） |

---

## 二、TDD 流程规范

```
每个 Phase：
  1. 写测试 → 编译失败（红灯）
  2. 写最小实现 → 编译通过（绿灯）
  3. 运行测试 → 验证通过
  4. 重构（如需）
  5. 提交
```

### 测试目录结构

```
test/
├── unit/                    # 单元测试
│   ├── test_event_dispatcher.cc
│   ├── test_rdma_endpoint.cc
│   ├── test_hello_message.cc
│   └── test_flow_control.cc
└── integration/             # 集成测试
    ├── test_multipart_send.cc
    └── test_flow_control_integration.cc
```

### 测试框架

- 使用 gtest（需添加依赖）
- 单元测试不依赖 RDMA 硬件
- 集成测试需要 RDMA 环境（标记 `RDMA_REQUIRED`）

---

## 三、迭代规划

| Phase | 名称 | 状态 | 文档 |
|-------|------|------|------|
| Phase 1 | FastRdmaEndpoint | 已完成 | [phases/phase1-rdmaendpoint.md](phases/phase1-rdmaendpoint.md) |
| Phase 2 | EventDispatcher | 已完成 | [phases/phase2-eventdispatcher.md](phases/phase2-eventdispatcher.md) |
| Phase 3 | MessageDispatcher | 已完成 | (inline in fast_rdma_endpoint.h / message_dispatcher.*) |
| Phase 4 | FastChannel | 设计中 | [phases/phase4-fastchannel.md](phases/phase4-fastchannel.md) |
| Phase 5 | FastServer | 设计中 | [phases/phase5-fastserver.md](phases/phase5-fastserver.md) |

> **已完成**：Phase 1-3 实现了完整的 TCP 握手、per-connection QP/CQ、EventDispatcher epoll、PollCq、MessageDispatcher 帧分发。
>
> **新设计**（2026-06-16）：Phase 4/5 采用 brpc 风格的回调结构。
> - FastRdmaEndpoint 新增 WriteQueue（`_write_head` 原子链表 + `StartWrite` / `KeepWrite` / `IsWriteComplete`）
> - FastChannel: `OnSerializeRequest` / `OnProcessResponse` 回调
> - FastServer: `OnProcessRequest` / `OnSerializeResponse` 回调
> - 移除 UniqueResource / SharedResource / librdmacm，全部改用 raw ibv_verbs + TCP 握手
> - MessageDispatcher 添加 DispatcherMode (kServer/kClient)
> - ResponseHead proto total_len 调为 field 1

---

## 四、构建与验收

每个 Phase 完成后必须：
1. **添加新源文件**和**单元测试**到 CMakeLists.txt
2. **编译验证**：`cd build && make`，无警告
3. **运行测试**：`./unit_tests` 或 `ctest --output-on-failure`，全部通过
4. 代码符合编码规范，文档更新（如有新增接口）

---

## 五、协议定义

### HelloMessage (32B)

```cpp
struct HelloMessage {
    uint16_t hello_ver = 1;      // light-rpc 协议版本
    uint16_t impl_ver = 1;       // 1=RDMA, 0=TCP 降级
    uint32_t block_size = 8192;  // Medium 分块大小
    uint16_t sq_size = 32;       // 本端 SQ 大小
    uint16_t rq_size = 32;       // 本端 RQ 大小
    uint16_t lid;                // IB LID
    uint8_t  gid[16];            // RoCE GID
    uint32_t qp_num;             // 本端 QP 号
};
```

### 交换方式

rdma_cm private_data（32B < 56B 上限）

---

## 六、CMakeLists 配置

```cmake
# 添加 gtest 依赖
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# 公共源文件
set(COMMON_SOURCES
    src-common/fast_block_pool.cc
    src-common/fast_define.cc
    src-common/fast_iobuf.cc
    src-common/fast_verbs.cc
)

# 端点层源文件
set(ENDPOINT_SOURCES
    src-endpoint/fast_channel.cc
    src-endpoint/fast_server.cc
    src-endpoint/event_dispatcher.cc
    src-endpoint/fast_rdma_endpoint.cc
)

# 单元测试
file(GLOB TEST_SOURCES test/unit/*.cc)

add_executable(unit_tests ${TEST_SOURCES})
target_link_libraries(unit_tests fastrpc ${GTEST_LIBRARIES} pthread)
add_test(NAME UnitTests COMMAND unit_tests)
```

---

## 七、流控设计

### 窗口变量

| 变量 | 含义 | 初值 |
|------|------|------|
| sq_window_size | 本端 SQ 剩余槽位 | 29 |
| remote_rq_window_size | 对端 RQ 槽位估计 | 29 |
| _sq_imm_window_size | Pure ACK 槽位 | 3 |
| _new_rq_wrs | 累积 free RQ | 0 |

### 窗口容量

```
local_window_capacity = sq_size - _sq_imm_window_size = 32 - 3 = 29
```

### Pure ACK 触发

```
_sq_imm_window_size > 0 && _new_rq_wrs > 29 / 2
```

---

