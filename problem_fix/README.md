# 问题分析与修复计划

本目录记录问题依据、修复方案、实施顺序和验收结果。计划中的待办不代表修复已经完成。

| 文档 | 范围 | 状态 |
|---|---|---|
| [RDMA endpoint 并发问题与修复计划](rdma-endpoint-concurrency.md) | 事件分发、CQ 处理、发送队列、等待唤醒、资源生命周期 | R01/R02/R03 队列协议、R04 生命周期、R05 注册表已修复（R04/R05 采用 brpc VersionedRefWithId/IOEventData）；下一项 R06，完整失败通知及实机验收待完成 |
