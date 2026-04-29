# Light-RPC 项目背景

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

**消息路径判断基于 total_length（字节数），而非 IOBuf ref count 或 SGE 数量**。recv 缓冲区预分配大小为 msg_threshold (2MB)。

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
