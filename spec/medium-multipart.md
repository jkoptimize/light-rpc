# Spec: Medium 消息分块 + 双窗口流控 + Blocking

> 更新：2026-05-24
> 模式：TDD (Test-Driven Development)

---

## 一、整体设计大纲

### 1.1 目标

解决 Medium 消息（200B ~ 2MB）的两个问题：
1. recv buffer 仅 8KB（max_sge=1），大消息装不下
2. IOBuf block 数量超过 MAX_SGE=32 时 SGE 越界

### 1.2 核心原则

| 原则 | 说明 |
|------|------|
| Medium 路径 | RDMA SEND/RECV 双边（非两阶段 RDMA WRITE） |
| 无新消息类型 | 现有帧头 `[total_len][meta_len]` 足够 |
| per-connection 顺序性 | RC 模式保序，无需追踪 rpc_id |
| per-connection 封装 | 通过 `RdmaEndpoint` 类管理 |
| 固定配置 | sq_size = rq_size = 32，block_size = 8KB |
| 非阻塞 EnqueueSend | io_context 线程入队即返 |
| SQ 满时阻塞 | OneWayButex（mutex + cond） |
| TDD | 每个 Phase 先写测试，后实现 |

### 1.3 架构变更

| 组件 | 原设计 | 新设计 |
|------|--------|--------|
| QP/RQ | 共享 SRQ | per-connection 独立 |
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
│   ├── test_one_way_butex.cc
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

| Phase | 名称 | 文档 |
|-------|------|------|
| 1 | OneWayButex | [phases/phase1-onewaybutex.md](phases/phase1-onewaybutex.md) |
| 2 | EventDispatcher | [phases/phase2-eventdispatcher.md](phases/phase2-eventdispatcher.md) |
| 3 | RdmaEndpoint | [phases/phase3-rdmaendpoint.md](phases/phase3-rdmaendpoint.md) |
| 4 | UniqueResource | [phases/phase4-uniqueresource.md](phases/phase4-uniqueresource.md) |
| 5 | SharedResource | [phases/phase5-sharedresource.md](phases/phase5-sharedresource.md) |
| 6 | FastChannel | [phases/phase6-fastchannel.md](phases/phase6-fastchannel.md) |
| 7 | FastServer | [phases/phase7-fastserver.md](phases/phase7-fastserver.md) |

---

## 四、CMakeLists 更新规范

每个 Phase 完成后必须：
1. **添加新源文件**到对应的源文件列表
2. **添加单元测试**到测试目标
3. **编译验证**：`cd build && make`
4. **运行测试**：`./unit_tests` 或 `ctest --output-on-failure`

---

## 四、协议定义

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

## 五、CMakeLists 配置

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
    src-common/one_way_butex.cc          # Phase 1
)

# 端点层源文件
set(ENDPOINT_SOURCES
    src-endpoint/fast_channel.cc
    src-endpoint/fast_server.cc
    src-endpoint/event_dispatcher.cc    # Phase 2
    src-endpoint/rdma_endpoint.cc       # Phase 3
)

# 单元测试
file(GLOB TEST_SOURCES test/unit/*.cc)

add_executable(unit_tests ${TEST_SOURCES})
target_link_libraries(unit_tests fastrpc ${GTEST_LIBRARIES} pthread)
add_test(NAME UnitTests COMMAND unit_tests)
```

---

## 六、流控设计

### 窗口变量

| 变量 | 含义 | 初值 |
|------|------|------|
| sq_window_size | 本端 SQ 剩余槽位 | 29 |
| remote_rq_window_size | 对端 RQ 槽位估计 | 29 |
| _sq_imm_window_size | Pure ACK 槽位 | 3 |
| _new_rq_wrs | 累积 free RQ | 0 |

### 窗口容量

```
local_window_capacity = min(32, 32) - 3 = 29
```

### Pure ACK 触发

```
_sq_imm_window_size > 0 && _new_rq_wrs > 29 / 2
```

---

## 七、编码规范（复用）

- 命名：PascalCase 类、下划线变量、成员 `_` 后缀
- 缩进：2 空格
- 错误处理：CHECK 宏
- 头文件：`#pragma once`

---

## 八、验收标准

每个 Phase 完成后：
1. 单元测试全部通过
2. 编译无警告
3. 代码符合编码规范
4. 文档更新（如有新增接口）