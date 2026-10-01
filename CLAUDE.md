# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## brpc 移植与修复原则（必须遵守）

- **默认保留 brpc 原实现。** 算法、数据结构、原子操作及其内存序、等待方式和错误处理流程均需对照实际源码；先理解每处细节的用途与并发前提，再制定方案。
- **只有明确的问题场景才允许偏离原实现。** 必须说明涉及的源码、触发条件或执行时序、实际后果，以及为什么原方案在本项目目标环境下无法满足要求。区分已复现问题、代码可推导的问题和尚未证实的理论顾虑。
- 不得仅以“更保险”“更符合通用写法”或“可能存在风险”为理由，擅自增加 atomic/mutex、加强内存序、改写调度或扩大基础设施。理论上的可移植性顾虑不能直接当作当前平台的故障证据；硬件行为也不能直接代替语言层面的同步证明。
- 优先补齐移植遗漏的原版协议和依赖。确需采用不同方案时，先说明最小改动、与原版的差异、正确性依据和性能影响，再按用户授权范围实施；已有明确授权无需重复确认。
- 修改后验证具体问题场景并记录结果。测试通过不代表所有平台安全，未做基准测试不得声称性能完全等价或额外开销可以忽略。当前范围仅考虑 Linux，不为未要求的平台主动增加机制。

## RDMA Verbs 规范约束（底线）

当**审视代码、讨论方案、编写实现**时，涉及任何 RDMA verbs 的使用（`ibv_*`），必须主动参考并遵循 IB verbs 标准规范（InfiniBand Architecture Specification, RDMA Protocol Verbs specification）。这包括但不限于：

- `qp_access_flags` 的有效值和语义（`IBV_ACCESS_REMOTE_WRITE`、`IBV_ACCESS_REMOTE_READ` 等）
- `ibv_reg_mr` 中 `ibv_access_flags` 的含义（`IBV_ACCESS_LOCAL_WRITE`、`IBV_ACCESS_REMOTE_WRITE` 等）
- `wr.imm_data` 与 `wc.imm_data` 的协议行为
- RDMA WRITE / SEND / RECV 操作的 CQE 语义和顺序保证
- RC 传输层的可靠性语义（transport ACK、send WC 含义）
- QP 状态机转换规则（RESET→INIT→RTR→RTS）
- `rnr_retry`、`min_rnr_timer` 等参数的含义和影响

**不可臆断或凭经验猜测 verbs 行为，必须对照规范确认。**

## 单元测试规则

- **不含 RDMA 接口的纯逻辑**：可以写单元测试（如 HelloMessage 序列化、流控窗口计算、HandleCompletion SEND WC 处理）
- **包含 RDMA 接口（ibv_post_send / ibv_poll_cq / ibv_post_recv 等）的场景**：不写单元测试
- 生产类中只为单元测试暴露的辅助方法，必须加注释 `// For unit tests only` 说明

## 用户偏好
- **身份**: 开发者，专注 RPC/RDMA 高性能网络
- **语言**: 中文交流
- **风格**: 简洁直接，不喜欢冗长总结；代码修改精准，不添加无关功能

## 构建与测试

### 依赖项
- **RDMA**: libibverbs, librdmacm (InfiniBand/RoCE 网络支持)
- **Boost**: Boost.Asio (异步事件循环)
- **Protobuf**: Protocol Buffers (RPC 接口定义)

### 构建命令
```bash
# 配置项目（首次运行）
cd build && cmake ..

# 编译（或重新编译）
cd build && make

# 构建产物
# - libfastrpc.a: 静态库
# - build/client: 客户端测试程序
# - build/server: 服务端测试程序
```

### 运行测试
```bash
# 启动服务端（在服务端机器）
cd build && ./server

# 启动客户端（在客户端机器，传入服务端IP）
cd build && ./client <server_ip>

# 测试特点
# - 自动测试多种消息大小：32B ~ 1MB
# - 自动测试多种并发：1 ~ 48 线程
# - 输出 QPS、中位延迟、P99 延迟
```

### Proto 文件生成
```bash
# 内部 RPC 协议（自动生成到 build/ 目录）
proto/fast_impl.proto → build/fast_impl.pb.{h,cc}

# 测试服务协议（自动生成到 build/ 目录）
test/test.proto → build/test.pb.{h,cc}

# 修改 proto 后需要重新运行 cmake && make
```

---

## 项目定位
**light-rpc** (build 名: FAST-RPC) 是一个基于 RDMA (Remote Direct Memory Access) 的高性能 RPC 框架，目标是将 RPC 延迟降低到微秒级甚至更低。它通过 InfiniBand/RoCE 网络实现零拷贝数据传输，完全绕过内核网络栈。

## 核心技术栈
- **传输层**: RDMA verbs (libibverbs) + RDMA CM (rdmacm)
- **RPC 接口**: Google Protocol Buffers (实现 `RpcChannel`)
- **异步处理**: Boost.Asio io_context
- **语言/编译**: C++17, CMake

## 核心设计思想

### 1. 消息大小分类发送（三条路径）
| 路径 | 判断条件 | RDMA 操作 | 特点 |
|------|----------|-----------|------|
| Inline | `total_length <= max_inline_data` (200B) | `IBV_WR_SEND_WITH_IMM` (inline) | 零拷贝，post_send 返回即硬件接收 |
| Medium | `total_length < msg_threshold` (2MB) | scatter-gather SEND | 零拷贝，IOBuf scatter-gather，IOBufAsZeroCopyOutputStream |
| Large | `total_length >= msg_threshold` (2MB) | 两阶段: SEND(NotifyMessage) + RDMA_WRITE | 单边，LargeBlockAlloc，零额外拷贝 |

**消息路径判断基于 total_length（字节数），而非 IOBuf ref count 或 SGE 数量**。recv 缓冲区预分配大小为 msg_threshold (8KB)。

### 2. 大消息两阶段协议
```
客户端                                    服务端
   │                                        │
   │  SendInline(NotifyMessage) ───────────►│ imm_data=FAST_NotifyMessage
   │                                        │  LargeBlockAlloc(recv_buf)
   │                                        │  recv_buf[total_len] = '0'
   │                                        │
   │◄─ WriteInline(AuthorityMessage) ──────│ remote_key, remote_addr
   │                                        │
   │  RDMA_WRITE(entire_frame) ───────────►│ 单次 WR，offset=0
   │                                        │
   │                                        │  写入 flag='1'
   │  等待 flag=='1'                        │
   │  处理响应                              │
```

### 3. LargeBlock TLS 缓存
- 大消息 buffer 来自 `LargeBlockAlloc()`：posix_memalign + ibv_reg_mr
- TLS cache（最多 8 块）：best-fit 分配，best-fit 插入，LRU 淘汰
- 线程退出自动 dereg_mr + free via ThreadExitHelper

### 4. 内存池 (Block Pool) — fast_block_pool
- 三种块大小: 8KB / 64KB / 2MB (`g_block_size[BLOCK_SIZE_COUNT]`)
- TLS 缓存 8KB 块减少竞争: `RDMA_MEMPOOL_TLS_CACHE_NUM = 128`
- Region 扩展机制: 最多 16 个 Region，每个 Region 默认 1GB
- 分配: 类型0优先TLS，其他随机选择bucket降低锁竞争
- 释放: 类型0优先放TLS，满时一半归还全局链表

### 5. IOBuf — fast_iobuf
- SmallView: ≤2 个块引用，栈上优化（无堆分配）
- BigView: >2 个块引用，堆分配动态数组
- Block 引用计数 + move 语义，避免数据复制
- TLS Block 链: 每线程最多缓存 8 个未满 block 复用
- 零拷贝序列化: `IOBufAsZeroCopyOutputStream` / `IOBufAsZeroCopyInputStream`
- 用于 Medium 消息路径（scatter-gather SEND）

### 6. CQ 混合轮询策略
- busy-spin (poll_times < 1000) → yield (poll_times < 2000) → 阻塞等待 IBV 事件
- `cq_poll_min_times = 1000` 切换阈值

### 7. 选择性 Signaling
- `#ifdef TEST_SELECTIVE_SIGNALING`: 每 16 次操作生成一次 CQE
- 目的: 减少 CQ 轮询和 WC 生成开销
- **重要**: `ibv_post_send` 返回成功只代表 WR 被写入硬件 queue，数据 buffer 不能回收，必须等 WC 返回
- `wr_id = send_counter` 用于关联 WC 和具体 buffer

### 8. Server 架构 — FastServer
- 多 Poller 线程池 (默认 CPU核心数/8)
- 共享 SRQ (Shared Receive Queue) 减少服务端口资源
- `conn_id_map_`: SafeHashMap<qp_num → rdma_cm_id*>
- 服务端 CQ 大小: `min_cqe_num = 512`, SRQ recv WR: `max_srq_wr = 512`
- 大消息响应使用 `SharedResource::LargeBlockAlloc()` + LargeBlock TLS cache

### 9. Client 架构 — FastChannel
- 每连接独立 QP/CQ (`UniqueResource`)
- 客户端 CQ 大小: `min_cqe_num = 32`, recv WR: `max_send_wr = 32`
- `ibvsend_client_addrs`: 保存已发送 WR 的 buffer 信息，等 WC 返回后归还
- 大消息请求使用 `UniqueResource::LargeBlockAlloc()` + LargeBlock TLS cache

## 关键源码位置
- `src-endpoint/fast_server.cc` — 服务端 CQ 轮询、请求路由、ReturnRPCResponse（大消息路径）
- `src-endpoint/fast_channel.cc` — 客户端 RPC 调用，Inline/Medium/Large 分支
- `src-resource/fast_shared.cc` — 服务端 RDMA 资源 + LargeBlock TLS cache (SharedResource)
- `src-resource/fast_unique.cc` — 客户端 RDMA 资源 + LargeBlock TLS cache (UniqueResource)
- `src-resource/fast_resource.cc` — FastResource 基类实现
- `src-common/fast_block_pool.cc` — RDMA 内存池
- `src-common/fast_iobuf.cc` / `inc/fast_iobuf.h` — 零拷贝缓冲区 (IOBuf)
- `src-common/fast_verbs.cc` — RDMA 操作封装
- `proto/fast_impl.proto` — 内部 RPC 协议定义

## 重要常量（定义在 `inc/fast_define.h` 和 `src-common/fast_define.cc`）
- `max_inline_data = 200B` — inline 路径阈值
- `msg_threshold = 2MB` — Medium/Large 消息分界阈值（recv 缓冲区大小）
- `MAX_SGE = 32` — scatter-gather 数组大小上限
- `timeout_in_ms = 1000` — RDMA 连接超时
- `listen_backlog = 200` — 服务端 listen backlog
- `cq_poll_min_times = 1000` — 轮询策略阈值
- `kMaxCachedLargeBlocks = 8` — TLS LargeBlock cache 上限
- `RDMA_MEMPOOL_TLS_CACHE_NUM = 128` — 内存池 TLS 缓存数量

## 关键结构体
- `AddressInfo` (`inc/fast_define.h`): 保存 buffer 地址和类型，用于 WC 清理
  - `BLOCK_ADDRESS`: 来自 BlockPool 的 block，释放用 `ReturnOneBlock`
  - `LARGE_BLOCK_ADDRESS`: LargeBlockAlloc 的大块，释放用 `ReturnLargeBlock`

## 编码规范
- 类名: PascalCase，以 `Fast`/`IOBuf` 开头
- 方法/变量: 下划线分隔小写
- 成员变量: 以 `_` 结尾
- 缩进: 2 空格
- 错误处理: `CHECK()` 宏，失败 exit
- 禁止裸 `new/delete` 用于 RDMA 相关内存
- 每次修改编码完成后必须编译通过

---

## 开发模式：TDD (Test-Driven Development)

当前采用 TDD 模式进行功能开发，具体规范见 `docs/works/spec/medium-multipart.md`。

### TDD 流程
```
每个 Phase：
  1. 写测试 → 编译失败（红灯）
  2. 写最小实现 → 编译通过（绿灯）
  3. 运行测试 → 验证通过
  4. 重构（如需）
  5. 提交
```

### 测试框架
- 使用 gtest
- 单元测试不依赖 RDMA 硬件
- 集成测试需要 RDMA 环境（标记 `RDMA_REQUIRED`）

### 当前迭代规划
| Phase | 名称 | 文档 |
|-------|------|------|
| 1 | FastRdmaEndpoint | [docs/works/spec/phase1-rdmaendpoint.md](docs/works/spec/phase1-rdmaendpoint.md) |
| 2 | EventDispatcher | [docs/works/spec/phase2-eventdispatcher.md](docs/works/spec/phase2-eventdispatcher.md) |
| 3 | MessageDispatcher | 新增 — 简化版消息分发器 |
| 4 | FastChannel | [docs/works/spec/phase4-fastchannel.md](docs/works/spec/phase4-fastchannel.md) |
| 5 | FastServer | [docs/works/spec/phase5-fastserver.md](docs/works/spec/phase5-fastserver.md) |

> Phase 3/4 (UniqueResource / SharedResource) 已移至 `docs/works/spec/deprecated/`，后续可能移除。

### CMakeLists 更新规范
每个 Phase 完成后必须：
1. **添加新源文件**到对应的源文件列表
2. **添加单元测试**到测试目标
3. **编译验证**：`cd build && make`
4. **运行测试**：`./unit_tests` 或 `ctest --output-on-failure`
