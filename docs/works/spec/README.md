# 迭代规格与实现计划

> **注意**：本目录下的文档属于**旧方案**的迭代规格与实现计划。随着架构演进（如改用 brpc `VersionedRefWithId`/`IOEventData`、双 QP 控制面/数据面、butex 等待唤醒等），部分内容已与最新代码实现不一致。阅读时请对照当前源码，不要以本文档为准反向推导最终设计。

## 索引

### 规格

| 文档 | 内容 |
|------|------|
| [Medium 消息分块迭代总纲](medium-multipart.md) | Medium 分块 + 双窗口流控 + Blocking，多 Phase 迭代 |
| [多长度消息 / 双 QP 消息分类架构](multi-length-message.md) | Control QP + Data QP 消息分类设计 |
| [Phase 1: FastRdmaEndpoint](phase1-rdmaendpoint.md) | per-connection 端点封装 |
| [Phase 2: EventDispatcher](phase2-eventdispatcher.md) | epoll 事件分发器 |
| [Phase 4: FastChannel](phase4-fastchannel.md) | 客户端端点层 |
| [Phase 5: FastServer](phase5-fastserver.md) | 服务端端点层 |

### 实现计划

| 文档 | 内容 |
|------|------|
| [双 QP 实现计划](2026-07-05-dual-qp-implementation.md) | 控制面/数据面双 QP 扩展的任务拆分 |
| [Phase 1: FastRdmaEndpoint 实施计划](2025-05-24-phase1-rdmaendpoint.md) | 早期 phase1 实施计划 |

### 已废弃

| 文档 | 内容 |
|------|------|
| [Phase 3: UniqueResource](deprecated/phase3-uniqueresource.md) | 已废弃 |
| [Phase 4: SharedResource](deprecated/phase4-sharedresource.md) | 已废弃 |
