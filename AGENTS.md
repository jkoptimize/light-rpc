# AGENTS.md

本文件补充 CLAUDE.md，说明项目定位与移植决策准则。CLAUDE.md 中的“brpc 移植与修复原则”“RDMA Verbs 规范约束”仍然有效，两者冲突时以 CLAUDE.md 的移植底线为准。

## 项目定位

light-rpc（构建名 FAST-RPC）是一款基于 brpc RDMA 发送语义的轻量级 RPC 通信框架。

- 实现上参考 brpc 的实现流程，同时精简压缩 brpc 中的部分实体（如 Socket、RdmaEndpoint、Controller 等）。
- 目的是在 brpc 基础实现之上进行改造，面向 RDMA 发送场景做能力扩展（例如控制面 / 数据面双通道）。
- 基础设施应尽量移植 brpc；维测、兼容性处理、不影响性能的额外能力可以不移植，以满足最终的性能压测对比目标。

> 注意：当前已有代码并非最终实现（例如仍在迁移 std::thread / pthread 到 bthread）。不要以当前实现为准反向推导最终设计。

## 移植与设计决策准则

分析或解决现有问题时，按以下顺序决策：

1. **已移植 brpc 基础设施**：直接使用，无需重新设计方案。
2. **未移植但已在移植计划中**：按计划中的方案处理，无需重新设计。
3. **不计划移植、属于 light-rpc 特有的问题**：参考 brpc 如何解决同类问题。
4. **借鉴方式**：
   - 若 brpc 方案内聚、无太多额外依赖，则直接借鉴（成套移植，保留原语义）。
   - 否则先给出 brpc 分析报告，再决定最小移植范围或替代方案，并说明与原版的差异、正确性依据与性能影响。

基础设施的判定以 `src-bthread/`（bthread_id、ResourcePool、butex、object_pool、list_of_abafree_id 等）以及 `src-common/` 当前已落地内容为准。

## RDMA CQ 模式范围

- 当前项目只实现基于 completion channel 的事件驱动 CQ；brpc `rdma_use_polling` 对应的 `polling_cq` 和 poller 线程池尚未移植，不要顺带增加 polling 专属分支或基础设施。只有用户明确启动 polling 模式工作时才纳入范围。
- 这不改变 verbs 的事件 ACK 义务：凡 `ibv_get_cq_event` 成功返回的事件都必须恰好 ACK 一次，包括意外返回的 CQ；completion channel 中尚未成功取出的通知不得猜测或计入 ACK 数。

## 连接模型范围

- 当前 light-rpc 移植 brpc 的**单连接（长连接）模型**：连接按需建立后持续复用；不关注短连接（`connect_on_create`）和连接池（`SocketPool`/`SocketMap` 复用）的实现，分析或重构时不要引入这两条路径的专属分支。
- 涉及连接建立、复用与关闭的语义以单连接模型为准。移植重构完成时，需核对当前连接模型实现是否与 brpc 单连接模型基本一致，并确认 Socket 的生命周期处理符合单连接预期（例如：连接失败后的重连/复用、关闭与回收时机、引用计数与 in-flight 请求的清理）。
