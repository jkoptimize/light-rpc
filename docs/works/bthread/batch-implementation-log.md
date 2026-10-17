# bthread 整改批次实施记录

## 2026-10-17：启动 B0

用户已审阅方案并授权开始实现。按 [实施计划](rpc-bthread-integration-plan.md) B0～B7 推进，保留已确认的裁剪边界。尚未开始 B1～B7，不能将本记录视为 bthread RPC 接入完成。

### 基线

- light-rpc HEAD：`193242810e746bc3924c80e2dd204c00c6ae4e0c`。
- brpc 参考 HEAD：`d688e7550be4b4c41b9a4dc55add2a2c75be1296`，本地源码 `/home/syt/Desktop/brpc/brpc`。
- 开始时运行时代码无工作区改动；实施计划已有修改，审计与多 Write 分析为未跟踪文档，均保留。
- 当前 `build/`：CMake Debug，`/usr/bin/c++`，C++17，Boost 1.74.0，Protobuf 3.12.4。生产改动前构建成功；新增测试后、修复前执行定向回归，见下文。
- 当前环境不存在 `/sys/class/infiniband`，未运行真实 RDMA client/server 或采集吞吐/延迟基线。此项仍未完成；后续性能比较须使用相同的修正后工具，并固定优化等级、硬件与负载，不能拿本次 Debug 纯逻辑测试充当性能结果。

### 本批代码

| 差异 | 修改 | 原版依据及保留范围 |
|---|---|---|
| D10 | CutInputMessage 区分完整、不完整、非法长度；非法长度以 EPROTO 传回 PollCq 既有失败分支。帧头使用流读取，支持跨 IOBuf BlockRef 和非对齐地址 | 对应 brpc InputMessenger::ProcessNewMessage 的解析结果分类；保留项目单协议整数返回值，不移植多协议协商。handler 返回值分类与分发调度留 B3/B4 |
| D13 | 先 cutn 分离 protobuf payload，再解析，附件保持为剩余 IOBuf | 对照 brpc policy/baidu_rpc_protocol.cpp::ProcessRpcResponse 的 res_buf/attachment 切分。未提前改 Controller 或全线协议；Channel 附件隔离仍留 B4 |
| D22 | atomic 停止标记、RAII 容器、有界延迟样本、单调时钟、成功/失败分开统计、空样本处理，输出 P50/P99/P999；耗时包括计入结果的在途请求排空 | 工具独立修正，不改生产调度。每线程一 Channel 暂时保留，单 Channel 多 outstanding 测量留 B7。成功样本每线程最多保留前 100 万条，输出截断标志；截断后百分位不是全程所有请求的分布 |

压测使用仅在工具内定义的 BenchmarkController 接收现有错误报告，B4 切换为生产 FastController；不引入生产兼容层或主动取消。响应内容检查也会计入失败统计，检查在单次延迟计时结束后执行，但会影响整体工具吞吐，后续对比必须统一工具行为。

此次不修改 verbs、队列算法、引用计数和内存序。payload 切分保留 IOBuf 引用语义，不复制正文；帧头读取只拷贝四字节。未做性能测量，不声称与原版性能等价。

### 验证

- 新增纯逻辑回归：非法短长度与不完整帧区分、分块帧头、解析错误向调用方传播、有/无附件响应的正文解析与附件完整性。使用普通 user-data IOBuf，不初始化 RDMA 硬件。
- 修复前执行 `build/unit_tests --gtest_filter='MessageDispatcher.*:ChannelLifecycle.ResponsePayloadExcludesAttachment'`：8 项中 4 项失败，实际复现上述缺陷。
- 修复后相同定向回归：8/8 通过。
- `cmake --build build -j 4` 成功。
- `ctest --test-dir build --output-on-failure --timeout 60`：7/7 测试目标通过，涵盖单测、写队列、窗口唤醒、bthread runtime 与日志构建模式。
- 硬件故障注入、RDMA 性能基线和压测运行结果尚未验证。B0 代码修复已落地，硬件基线待采集；B1 为下一代码批次。
