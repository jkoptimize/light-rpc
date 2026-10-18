# bthread 整改批次实施记录

当前进度（2026-10-18）：B0 代码与 B1 已落地并通过构建/回归；B0 硬件性能基线仍待采集，B2～B7 尚未实施。下文保留各批实施时的历史状态。

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

### B0 补充：按 brpc 补齐 IOBuf::copy_to

根据用户复核，移除 CutInputMessage 内临时编写的输入流遍历，改为调用 `copy_to(&be, sizeof(be))`，对应 brpc `ParseRpcMessage` 读取局部帧头的方式。

- 移植上述 brpc 基线 `butil/iobuf.cpp::IOBuf::copy_to(void*, size_t, size_t)`：按 pos 跳过 BlockRef，再依次复制，返回实际复制字节数；默认 n 为 size_t 最大值，不消费输入、不改变引用所有权。
- 原版 `iobuf::cp` 仅转发 memcpy，项目直接调用 memcpy，不额外增加纯转发函数。算法与边界处理保留原版；本次仅补齐所需 raw-buffer 重载，不扩展字符串等接口。
- 保留原版防止 IOBuf 指针误入 void* 重载的编译期约束，使用本项目 C++17 的 deleted overload 表达，不引入旧编译器兼容分支。
- 新增纯逻辑测试覆盖 SmallView/BigView、非零 BlockRef offset、跨块位置、默认长度、零长度、超范围位置、超长读取、输出边界及输入长度/资源寿命保持。先观察缺少接口导致的编译失败，再完成实现。
- 本次 `cmake --build build -j 4` 成功；`ctest --test-dir build --output-on-failure --timeout 60` 7/7 目标通过；`git diff --check` 通过。未修改 verbs 或开展性能测试。

## 2026-10-18：B1 发送节点与队列（D01、D03）

开始时工作区干净，light-rpc HEAD 为 `9d6e4d45ec8250b32550f3020c8982d7e766c338`。brpc 参考仍为 `d688e7550be4b4c41b9a4dc55add2a2c75be1296`。

### 原版对应与改动

- 对照 `Socket::Write` 的 get_object 与每次获取后初始化，以及 `Socket::ReturnSuccessfulWriteRequest/ReturnFailedWriteRequest`：StartWrite 改用已移植的 `butil::get_object<WriteRequest>`，成功路径检查 data 已空后归还，失败路径先 clear 再归还。替换 StartWrite、KeepWrite、失败排空的所有节点 delete；池归还后不再读取节点。
- 每次获取后显式设置 `next=UNCONNECTED`，不依赖首次构造的默认成员初始化。采用原版 `BAIDU_CACHELINE_ALIGNMENT`；本次实际测得 `sizeof(WriteRequest)=64`、`alignof(WriteRequest)=64`，池返回地址也满足对齐。
- 对照 `Socket::StartWrite`，将 `_write_head.exchange(req, release)` 直接放在 StartWrite，移除 PublishWriteRequest。保留 next 衔接、消费者反转、IsWriteComplete 的 acquire CAS、最后节点保留及并发发布后的失败检查，未加强原子内存序或新增锁。
- 删除纯转发 WriteError，直接读取既有 error()。保留发布前后错误检查的原有作用。
- 测试 peer 直接操作发布原子，不为测试保留生产包装函数；测试节点统一使用同一个对象池，避免向对象池归还 new 分配的节点。

### 裁剪与未实施部分

本批节点保留实际使用的 data、next。Endpoint 回调上下文在 B2 成套引入；id_wait 及对应通知在 B4 引入，不预填尚未使用的状态。不增加每节点 Endpoint 智能指针、控制位、认证或 pipelining 字段。

`_pending_keepwrite_req`、当前异步连接和 BeforeRecycled 的兜底协议本批保留，B2 再完整替换；本批不修改 verbs、CQ 消费权、事件注销或后台恢复。减少逐请求堆分配是机制上的预期；64 字节布局和对象池保留内存也有成本，未实测吞吐或延迟，不宣称性能提升幅度。

### 验证

- `cmake --build build -j 4` 成功。
- `build/rdma_write_queue_tests --gtest_output=xml:/tmp/light-rpc-b1-write-queue.xml`：9/9 通过，包括新加的池复用、失败缓冲清理、64 字节对齐验证；原有 FIFO、UNCONNECTED 等待、并发失败排空、握手暂存失败、真实 StartWrite 同步建连失败回归保留。
- `ctest --test-dir build --output-on-failure --timeout 60`：7/7 测试目标通过，包含生命周期、写窗口唤醒和 bthread runtime。
- 全局检索确认生产及测试均不再使用 PublishWriteRequest、WriteError 或 WriteRequest 的 new/delete；`git diff --check` 通过。
- 上述队列和生命周期测试使用无 QP/CQ 的 Endpoint；未运行 RDMA 硬件集成或压测。下一批为 B2 建连与握手所有权协议。
