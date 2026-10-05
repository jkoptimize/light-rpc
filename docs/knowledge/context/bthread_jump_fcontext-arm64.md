# ARM64：bthread_jump_fcontext 如何保存、切换和恢复协程

本文只解释项目 `context.cpp` 的 **Linux ARM64 / AArch64** 分支。建议先读 [bthread_make_fcontext](../../docs/knowledge/bthread_make_fcontext-arm64.md)，理解寄存器基础和初始栈布局。

核对日期：2026-10-04。该汇编分支与本机 brpc 对应实现一致。本文按源码推导执行过程，未做 ARM64 硬件执行或性能验证。

## 1. 一句话说明它做什么

`bthread_jump_fcontext` 把当前执行流需要保留的寄存器写入当前栈，记录保存区地址；然后把 SP 换成目标上下文的保存区地址，恢复目标寄存器，并跳到目标执行位置。

```text
保存 A → *ofc = A 的保存区地址
切换 SP → 指向 B 的保存区
恢复 B → 跳到 B 的入口或上次挂起位置
```

它没有复制整条栈，也不调用内核调度器。A 的普通局部变量和调用链仍留在 A 的栈内存中；切换时只读写一个固定大小的寄存器保存区。

## 2. 接口和 ARM64 参数位置

声明见 [context.h](../../src-bthread/context.h)：

```cpp
intptr_t bthread_jump_fcontext(
    bthread_fcontext_t* ofc,
    bthread_fcontext_t nfc,
    intptr_t vp,
    bool preserve_fpu = false);
```

| 参数 | 寄存器 | 含义 |
|---|---|---|
| `ofc` | x0 | 用来保存旧 context 指针的变量地址 |
| `nfc` | x1 | 目标 context 指针，即目标保存区地址 |
| `vp` | x2 | 传给目标执行流的整数/指针大小数据 |
| `preserve_fpu` | w3 的参数位置 | 本 ARM64 分支不根据它决定是否保存 d8～d15 |
| 返回值 | x0 | 将来恢复本次调用时，由恢复者的 vp 提供 |

重点区别：`ofc` 是“指针变量的地址”，`nfc` 是“保存区地址”。项目调用见 [stack_inl.h](../../src-bthread/stack_inl.h)：

```cpp
bthread_jump_fcontext(&from->context, to->context, 0);
```

这里 `&from->context` 接收新的保存区地址，`to->context` 已经由 make 或先前的 jump 准备好。main stack 在首次切走时也能通过这个过程得到 context，不要求每条旧栈预先调用 make。

## 3. 为什么只保存部分寄存器

普通函数调用中，调用者不能假定所有寄存器在调用后都不变。编译器会把跨调用仍需要的数据放入栈中，或放到约定由被调用者保留的寄存器中。

本实现保存 x19～x29，并保存 LR、SP 所对应的恢复信息；还保存 d8～d15。AAPCS64 的基础调用约定要求保留 x19～x29、SP，以及 v8～v15 的低 64 位；其他调用者保存寄存器中的活跃值，由调用者负责保护。[Arm 官方 AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)

因此，x0～x17 等不会在这里被逐个保存。x18 具有平台相关含义，不能把它一概当成通用持久寄存器；本实现没有保存它。

这是在明确的函数调用边界进行的协作式切换，不是在任意机器指令位置抢占任务。不要把它等同于操作系统中断处理时的完整现场保存。

## 4. 必要汇编语法

| 指令示例 | 意义 |
|---|---|
| `sub sp, sp, #0xb0` | SP 减 176，向低地址预留空间 |
| `stp x19, x20, [sp, #0x40]` | 把两个 64 位值写入 SP+64、SP+72 |
| `ldp x19, x20, [sp, #0x40]` | 从对应两个位置读取寄存器 |
| `str x30, [sp, #0xa0]` | 向 SP+160 写入 8 字节 |
| `ldr x4, [sp, #0xa0]` | 从 SP+160 读取 8 字节 |
| `mov sp, x1` | 用 x1 的值替换当前 SP |
| `ret x4` | 从 x4 取得跳转地址；不自动调整 SP，也不改写 LR |

此处 `stp/ldp` 没有 `!` 或后索引参数，因此不会顺带改变 SP。`dN` 是 64 位视图，所以 `stp d8,d9` 同样存 16 字节，不是存两个 128 位寄存器。

## 5. 分阶段逐条读汇编

源码位于 [context.cpp](../../src-bthread/context.cpp) 的 `BTHREAD_CONTEXT_PLATFORM_linux_arm64` 分支。以下去掉文件作用域声明和被注释的条件判断，保留有效指令。

### 5.1 在旧栈建立保存区

```asm
sub sp, sp, #0xb0
```

假设进入 jump 时旧 SP 为 S_A，执行后：

```text
C_A = S_A - 0xb0
SP = C_A
```

这相当于建立 jump 自己的 176 字节栈帧；因为 176 是 16 的倍数，原有 16 字节对齐被保留。它不会分配额外栈内存，也不检测栈剩余容量。

### 5.2 保存 d8～d15

```asm
stp d8,  d9,  [sp, #0x00]
stp d10, d11, [sp, #0x10]
stp d12, d13, [sp, #0x20]
stp d14, d15, [sp, #0x30]
```

共保存 8 × 8 = 64 字节。源码中根据 w3 跳过这部分的 `cmp/b.eq` 已被注释，因此即使默认 `preserve_fpu=false` 也照常保存。

原注释解释：编译器可能把整数数据放进浮点寄存器跨函数调用保存。因此“业务没有浮点运算”不能成为跳过它们的依据。

这里只保存 d8～d15，不包括 v8～v15 的完整 128 位，也没有保存 FPCR/FPSR。不能把接口参数名理解成“完整浮点环境快照”。

### 5.3 保存通用寄存器、LR 与恢复位置

```asm
stp x19, x20, [sp, #0x40]
stp x21, x22, [sp, #0x50]
stp x23, x24, [sp, #0x60]
stp x25, x26, [sp, #0x70]
stp x27, x28, [sp, #0x80]
stp x29, x30, [sp, #0x90]
str x30,      [sp, #0xa0]
```

前六条保存 12 × 8 = 96 字节。x29 在 `0x90`，x30 在 `0x98`。最后再将 x30 写到 `0xa0`。

为什么 x30 存两次？调用 jump 时，调用指令已经把“调用后的继续执行地址”放入 LR。旧上下文中，恢复的 LR 和恢复执行地址恰好相同；新上下文中却不同：make 写入 `LR=finish`、`执行地址=fn`。统一格式允许 jump 用同一段恢复代码处理两者。

此处不是读取 jump 当前指令的 PC。存下的是调用者希望恢复后继续执行的位置。

### 5.4 把保存区地址写给调用者

```asm
mov x4, sp
str x4, [x0]
```

其效果是：

```cpp
*ofc = C_A;
```

`str` 的待存储通用寄存器操作数不能直接使用 SP，所以用临时寄存器 x4 中转。x0 此时仍是 `&from->context`，不是保存区本身。

context 不必另存“原 SP”：由 `C_A + 0xb0` 就能重建 S_A。每次挂起的调用深度可能不同，所以 `from->context` 会被更新，而不是永远指向 make 最初返回的位置。

### 5.5 改变 SP，开始恢复目标栈

```asm
mov sp, x1
```

此刻 `SP = C_B`。后续 `[sp, ...]` 的访问都指向 B 的保存区。

这是栈切换的关键指令，但它本身没有跳到 B 的代码：CPU 此刻仍在 jump 的恢复指令中执行。寄存器逐步恢复后，最后的 `ret x4` 才转移控制流。

原注释中的 “RSP” 是沿用的术语，ARM64 此处实际操作的是 SP；原注释的 “A2” 指第二个参数 x1。

### 5.6 恢复目标寄存器

```asm
ldp d8,  d9,  [sp, #0x00]
ldp d10, d11, [sp, #0x10]
ldp d12, d13, [sp, #0x20]
ldp d14, d15, [sp, #0x30]
ldp x19, x20, [sp, #0x40]
ldp x21, x22, [sp, #0x50]
ldp x23, x24, [sp, #0x60]
ldp x25, x26, [sp, #0x70]
ldp x27, x28, [sp, #0x80]
ldp x29, x30, [sp, #0x90]
```

与保存位置一一对应。若 B 曾经运行过，恢复的是它的历史值；若 B 是 make 构造的新上下文，除 make 显式写入的 LR/执行地址外，其余初始槽位没有规定值。

保存区完整布局见 [make 文档第 5 节](../../docs/knowledge/bthread_make_fcontext-arm64.md#5-两个函数共用的-176-字节布局)。前 160 字节是寄存器，随后 8 字节为执行地址，最后 8 字节是填充。

### 5.7 传递数据，恢复目标 SP，跳转

```asm
mov x0, x2
ldr x4, [sp, #0xa0]
add sp, sp, #0xb0
ret x4
```

逐条含义：

1. x0 接收本次切换的 vp。新入口把它当第一个参数；旧调用点把它当 jump 的返回值。
2. x4 取得 B 的入口或续执行地址。
3. SP 从 C_B 增至 C_B+176，撤销保存区占用，回到 B 挂起前的 SP；新上下文则到达对齐后的栈高端。
4. 跳转到 x4；LR 保持刚恢复的值。

为什么不用 `bl`？因为带链接跳转会覆盖 LR。新任务入口需要保留 make 设置的 finish 作为返回地址。

这里的 `add sp` 不是归还栈给对象池，更不是 `free`；它只调整寄存器。旧保存区字节还在内存中，随后可以被正常栈使用覆盖。

## 6. A → B → A：一次完整往返

用概念代码说明接口的数据传递，省略对象初始化：

```cpp
// A 中
intptr_t r = bthread_jump_fcontext(&ctx_A, ctx_B, 11);
// A 再次被恢复后，从这里继续

// B 的入口 fn(intptr_t arg) 中，arg == 11
bthread_jump_fcontext(&ctx_B, ctx_A, 22);
```

假设 A 进入 jump 时 `S_A=0x80000`，新建 B 的对齐栈高端 `T_B=0x10000`：

| 时刻 | SP / context | 执行位置 |
|---|---|---|
| A 调用 jump | SP=0x80000 | A 的 jump 入口 |
| A 保存完成 | ctx_A=0x7ff50 | A 栈上的保存区已写好 |
| 选择 B | SP=ctx_B=0xff50 | 仍在 jump 的恢复代码 |
| 首次进入 B | SP=0x10000，x0=11，LR=finish | B 的入口 fn |
| B 调用 jump | ctx_B 更新为 B 当前 SP−176 | 保存 B，并选中 ctx_A |
| 恢复 A | SP=0x80000，x0=22 | A 原先调用 jump 后的位置 |

因此 A 的 `r == 22`，不是 11。A 发出的 11 交给 B；后来恢复 A 的那次调用携带的 22 才成为 A 的返回值。

B 的那次 jump 此刻还没有在 B 的控制流中完成。未来恢复 B 时，它才从自己对应的调用后位置继续。

这不是恢复到旧 jump 中的 `mov sp,x1` 后面：恢复者直接执行保存区中的续执行地址，通常就是旧调用者的调用后地址。

## 7. 对应到 TaskGroup 的两条路径

### 7.1 目标上下文首次运行

```text
sched_to → jump_stack → bthread_jump_fcontext
  → 恢复 make 创建的保存区
  → task_runner(0)（ASan 构建可能先经过包装）
  → 处理 remained 回调
  → 执行当前 TaskMeta 的 fn
```

新任务没有历史挂起的 C++ 调用栈，因此从入口开始。

### 7.2 目标上下文以前运行过

```text
恢复旧寄存器和旧 SP
  → 回到目标任务之前的 bthread_jump_fcontext 调用后
  → jump_stack 返回（若被内联则没有独立包装栈帧）
  → 继续目标任务自己的 sched_to 后半段
  → 重新读取 tls_task_group，处理 remained 回调
  → 逐层回到原等待或调度调用处
```

不是回到本次唤醒它的 worker 那一份 sched_to 调用栈。当前执行的是目标任务自己保存的调用栈；若已迁移到另一 worker，C++ 层必须重新取得该 worker 的 TLS。

### 7.3 并非每次任务切换都会调用这段汇编

结束任务把栈直接转交给下一个新任务时，`ending_sched` 可以复用同一条栈，`task_runner` 循环继续处理新 TaskMeta。PTHREAD 类型任务也有主栈路径。因此“新任务”不总是意味着“新 make 的上下文”。

源码依据：[task_group.cpp](../../src-bthread/task_group.cpp) 的 `task_runner`、`ending_sched`、`sched_to`。

## 8. 它不负责什么

| 工作 | 实际负责位置 |
|---|---|
| 选择下一个任务 | TaskGroup / TaskControl 的调度逻辑 |
| 栈内存申请、guard | allocate_stack_storage |
| 任务退出与 keytable 清理 | task_runner |
| 延迟归还旧栈和 slot | remained 回调，例如 _release_last_context |
| bthread 局部存储、errno 等 | sched_to 等 C++ 路径 |
| pthread 迁移和任务交接同步 | 调度器队列及相关同步协议 |

汇编没有更换 pthread 的 TLS 基址，没有保存信号屏蔽字，也没有完整保存所有 ARM64 系统/扩展寄存器。这不是缺少一条普通 `stp` 就应当补齐的问题，而是这份基础切换接口本身的范围。

同一个 context 也不能被两个 worker 同时恢复：两者会使用同一条栈。`str x4,[x0]` 本身不是完整的跨线程发布协议，任务可运行状态和队列交接仍需调度器保证。

## 9. 常见误解与自检

- **“切栈就是复制整条栈。”** 实际是切 SP，栈内存留在原地。
- **“ret 一定回到刚才调用我的函数。”** 它从指定寄存器取得目标；这里用的是目标上下文地址。
- **“mov sp,x1 后已经执行目标业务代码。”** 还要恢复寄存器并执行最后的跳转。
- **“保存 LR 就够，不必单独留 PC 槽。”** 新上下文需要 fn 和 finish 两个不同地址。
- **“参数默认 false，就没有浮点保存。”** 本分支的条件判断已经被注释。
- **“旧任务函数结束即可立即 free 栈。”** 切换代码仍可能使用旧栈，必须在安全交接之后回收。

可自己推导：若旧 SP 为 `0x20000`，保存区应为 `0x1ff50`；恢复时加回 `0xb0` 得 `0x20000`。再检查 `0x98` 处的 LR 与 `0xa0` 处的执行地址在新旧上下文中的区别。

来源：[项目 context.cpp](../../src-bthread/context.cpp)、[context.h](../../src-bthread/context.h)、[stack_inl.h](../../src-bthread/stack_inl.h)、[task_group.cpp](../../src-bthread/task_group.cpp)。寄存器和调用约定参考 [Arm 官方 AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)。
