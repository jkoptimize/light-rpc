# bthread 日志基础设施修复

日期：2026-09-27。对照 brpc 提交 `d688e7550be4b4c41b9a4dc55add2a2c75be1296`。

## 范围

按本轮确认，优先处理限频日志、DCHECK 和日志参数惰性求值。后续已确认并裁剪无消费者的 `_nbthreads`、`_nworkers` 字段及更新，保留调度控制所需的 `_concurrency`；完整 bvar 移植继续暂缓。

完整 brpc 日志后端依赖 gflags、文件工具、线程设施、回溯、字符串处理等。本次迁移需要的宏和基础辅助实现，接到项目现有 stderr/字符串流后端；不宣称已完整移植日志库，也不宣称输出后端与 brpc 性能等价。

## 来源与依赖

| 当前实现 | 上游来源 | 适配 |
| --- | --- | --- |
| `inc/detail/fast_log_macros.h` 惰性求值宏、Voidify | `butil/logging.h` | 命名空间和内部宏前缀；保留 `?:` 与 `&` 表达式结构 |
| CHECK/DCHECK 和二元检查辅助函数 | `butil/logging.h` 普通构建分支 | 保留求值次数、条件、错误值格式；去掉仅供原输出后端使用的 SetCheck 标记 |
| EVERY_SECOND / FIRST_N / EVERY_N | `butil/logging.h` | 保留调用点静态状态、条件顺序、CAS/原子增量算法；使用等价 relaxed std::atomic 原语 |
| `gettimeofday_us()` | `butil/time.h` | 相同系统接口和微秒换算，使用本地命名空间以避开尚未闭合的 bthread 构建依赖 |
| 原子适配 | `butil/atomicops_internals_x86_gcc.h` 的 NoBarrier 接口约定 | strong CAS 返回旧值，increment 返回新值；编译期要求 32/64 位 atomic 无锁 |
| PLOG 错误捕获 | `butil/logging.cc` POSIX ErrnoLogMessage | 构造时保存 errno，流参数求值后再追加错误文本；继续使用项目后端 |

保留部分只依赖 C++17 标准库与 Linux `gettimeofday`，无悬空的 butil/bvar/gflags 引用。原 brpc 仓库未改动。

## 行为与调用点

- `LOG_IF` 条件为假、日志级别关闭或被限频时，流式参数不求值；`LOG_IF` 的条件也受日志级别短路控制。
- `CHECK` 始终求值，支持追加日志和 `CHECK_EQ/NE/LT/LE/GT/GE`；失败终止方式由原项目的 `exit` 改为 FATAL/`abort`。含实际操作的 CHECK 表达式没有被删除。
- DCHECK 遵循 `NDEBUG` / `DCHECK_ALWAYS_ON`；Release 补回此前漏掉的 `-DNDEBUG`。这也恢复标准 assert 在 Release 下关闭的常规行为。
- `TaskGroup::run_main_task` 的两处 CHECK 恢复为原版 DCHECK_EQ；BoundedQueue 两处 assert 恢复为 DCHECK。
- `push_rq`、`ready_to_run_remote` 恢复 LOG_EVERY_SECOND。
- 栈分配中的 malloc/mmap/mprotect 错误恢复 PLOG_EVERY_SECOND；页对齐异常恢复 LOG_ONCE。
- `fast::SetMinLogLevel()` 控制流式日志级别，默认 INFO，FATAL 始终开启。后续统一迁移了全部 23 处旧日志调用：错误日志使用 PLOG(ERROR) 保留 errno 信息，信息日志使用 LOG(INFO)，旧的 printf 风格宏定义已删除。

限频保持上游按墙钟、按调用点的算法：多线程共享该调用点的状态，CAS 成功者输出。墙钟回拨可能延后下一条日志，没有擅自改成单调时钟算法。

与上游相同，限频宏包含多条语句，二元 CHECK/DCHECK 使用内部 if；外层条件分支应使用花括号，同一行不要放两个同类限频宏。本次没有另行重写这些宏结构。

## 验证

- `test/logging/test_fast_log.cc`：13 项测试，在 Debug、Release、Release + DCHECK_ALWAYS_ON 三种模式下全部通过，共 39 次用例执行。
- 覆盖条件短路、级别过滤、流参数副作用、检查求值次数、致命退出、errno 捕获、限频边界、墙钟回拨、调用点隔离、16 线程竞争和只输出一次。
- 时间使用测试链接器 `--wrap=gettimeofday` 控制，生产代码没有测试开关；线程通过条件变量同时放行，不依赖 sleep 猜测时序。
- 独立回归运行已有基础库、容器、TLS、fast_utils、IOBuf 用户数据测试：43 项通过。
- 主项目 `fastrpc`、client、server 的 Debug/Release 编译及链接通过。没有执行依赖 RDMA 硬件的程序。
- 初次日志修复时，`fast_bthread` 仍受 `src-bthread/errno.h` 遮蔽系统头阻塞；后续已修复搜索路径，见下节。核心源文件尚未完整进入目标，基础库构建成功不代表调度核心完成集成。

复现日志测试：

```bash
cmake -S . -B /tmp/light-rpc-logging-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/light-rpc-logging-build --target fast_log_test_debug fast_log_test_release fast_log_test_forced -j4
ctest --test-dir /tmp/light-rpc-logging-build -R '^fast_log_test_' --output-on-failure
```

## errno.h 搜索路径修复（同日后续）

`src-bthread/CMakeLists.txt` 将公开的 `-I src-bthread` 改为 `-iquote src-bthread`。项目当前以双引号引用 bthread/butil 头，仍能正常解析；系统 `<errno.h>`、`<cerrno>`、`<mutex>` 不再搜索该目录。参数通过 PUBLIC 传递给 unit_tests 等消费者，避免冲突从库传播到使用方。

保留 `src-bthread/errno.h` 中的原版封装与 errno 重定义逻辑，没有重写 errno 实现，也没有添加硬编码的系统头路径。

验证：在 `/tmp/light-rpc-errno-build`（Debug）和 `/tmp/light-rpc-errno-release-build`（Release）完成全部现有目标构建，两套 CTest 均通过。每套包括 76 项项目单元测试和 13 × 3 次日志测试。源码、宏定义及文档示例中已无旧日志宏调用。

当前 fast_bthread 目标仅包含 time、fast_rand、thread_local、thread_key、murmurhash3 五个基础源文件；mutex、调度核心、errno.cpp 等仍需后续接入和验证。
