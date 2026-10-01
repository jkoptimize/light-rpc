# 工作计划与改造文档

记录移植、开发迭代、问题修复等「要做 / 正在做 / 已做」的工作类文档。与 [docs/knowledge](../knowledge/)（知识沉淀/分析）区分：本目录偏向计划、规格、进度与验收。

## 目录结构

| 子目录 | 内容 |
|--------|------|
| [bthread/](bthread/) | brpc bthread 移植相关（设计、计划、学习路径、日志修复） |
| [spec/](spec/) | 迭代规格与实现计划（**旧方案**，可能与最新代码不一致，见 [spec/README.md](spec/README.md)） |
| [problem-fix/](problem-fix/) | 具体问题的分析与修复记录 |

## 问题修复

| 文档 | 内容 |
|------|------|
| [RDMA endpoint 并发问题与修复计划](problem-fix/rdma-endpoint-concurrency.md) | 事件分发、CQ 处理、发送队列、等待唤醒、资源生命周期的并发问题清单与修复记录（R01～R08 已修复，O01 归入 bthread 移植） |
