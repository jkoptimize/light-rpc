# ARM64：bthread_make_fcontext 如何构造一个可启动的协程上下文

本文面向没有系统学习过汇编的读者，只解释项目 `context.cpp` 的 **Linux ARM64 / AArch64** 分支。建议先读本文，再读 [bthread_jump_fcontext](../../knowledge/context/bthread_jump_fcontext-arm64.md)。

核对日期：2026-10-04。该分支与本机 brpc 对应实现一致。以下是源码解释，不是修改方案；没有声称已经在 ARM64 硬件上运行验证。

## 1. 这个函数到底创建什么

源码：[context.cpp](../../src-bthread/context.cpp)，定位 `BTHREAD_CONTEXT_PLATFORM_linux_arm64` 下的 `bthread_make_fcontext`；声明见 [context.h](../../src-bthread/context.h)。

```cpp
typedef void* bthread_fcontext_t;
bthread_fcontext_t bthread_make_fcontext(
    void* sp, size_t size, void (*fn)(intptr_t));
```

它在**已经分配好的栈内存**中准备一小块“初始寄存器保存区”，返回这块保存区的地址。以后 `bthread_jump_fcontext` 按约定读取它，就能把一条从未运行过的栈当成“可以恢复的上下文”，跳到 `fn`。

它不分配栈、不创建 pthread、不执行 `fn`，也不把当前 CPU 的 SP 切到新栈。`make` 正常返回给调用者，真正的切换发生在 `jump`。

本项目调用路径是：

```text
StackFactory 的 Wrapper 构造函数
  → allocate_stack_storage：分配栈和可选 guard
  → bthread_make_fcontext(storage.bottom, storage.stacksize, entry)
  → 将返回值保存在 ContextualStack::context
```

可查看 [stack_inl.h](../../src-bthread/stack_inl.h) 的 `StackFactory`。普通构建中入口通常是 `TaskGroup::task_runner`；ASan 构建有入口包装。传入的不是业务 `m->fn`，业务函数由 `task_runner` 再调用。

## 2. 必要基础：寄存器、地址、栈

### 2.1 寄存器不是内存地址本身

寄存器是 CPU 内的小型存储位置，可以装整数，也可以装地址。`x0` 中装有地址，不等于 CPU 已经访问该地址。

```asm
mov x1, x0         // 复制数值：x1 = x0
ldr x1, [x0]       // 读内存：x1 = 地址 x0 处的 8 字节
str x1, [x0]       // 写内存：把 x1 的 8 字节写到地址 x0
```

方括号表示内存寻址；`#0xa0` 是立即数 160，偏移单位是字节。本文汇编代码块中的注释为讲解所加。

### 2.2 读懂这几个寄存器

| 名字 | 在本文中的作用 |
|---|---|
| `x0`～`x30` | 64 位通用寄存器 |
| `w0`～`w30` | 对应通用寄存器的低 32 位视图，不是另一组独立寄存器 |
| `sp` | 当前正在使用的栈指针 |
| `x29` / FP | 通常用作栈帧指针 |
| `x30` / LR | 保存函数返回地址 |
| PC | 当前指令位置；通过分支指令改变，不是这里可直接 `ldr` 的通用寄存器 |
| `d8`～`d15` | SIMD/浮点寄存器 `v8`～`v15` 的低 64 位视图 |

普通整数/指针参数依次用 `x0`、`x1`、`x2` 等传递；指针返回值放在 `x0`。这些属于调用约定，编译器和汇编函数必须遵守同一规则。

### 2.3 栈向低地址增长

设分配到的可用栈区间为 `[L, H)`，`H` 是高地址端的后一字节地址。新栈从高地址端开始使用，预留空间通常通过减小 SP 完成。注意项目名为 `storage.bottom` 的字段指向这里的 **H**，不要仅凭英文名字判断方向。

```text
低地址 L     可选 guard 在 L 以下
             可用栈空间
             ↓ 向低地址增长
高地址 H     storage.bottom（区间右端，不属于可用字节）
```

页对齐用于内存映射/保护，16 字节对齐用于 ARM64 调用约定，两者不是一回事。AAPCS64 要求函数接口处 SP 为 16 字节对齐，并对经 SP 的内存访问规定相应对齐要求。[Arm AAPCS64：栈规则](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)

### 2.4 普通调用为什么能返回

```asm
bl some_function  // 把下一条指令的地址放入 x30，再跳到函数
// 返回后继续执行的位置
```

被调用函数最后通常用 `ret`（默认目标为 `x30`）返回。ARM64 的 `bl` 不会自动把返回地址压入栈；需要跨嵌套调用保留 LR 时，通常由函数序言保存它。

`ret x4` 则从 x4 取得跳转目标，并不会把 x4 写入 LR，也不会自动调整 SP。后一个特征正是新协程能同时设置“入口”和“入口返回后的去处”的原因。

## 3. 函数入口时，参数在哪里

| C++ 参数 | 寄存器 | 实际含义 |
|---|---|---|
| `sp` | `x0` | 目标栈的高地址端；不是当前硬件 SP |
| `size` | `x1` | 栈大小；本 ARM64 实现不读取它做检查 |
| `fn` | `x2` | 首次启动上下文时执行的入口函数地址 |

这里的 C++ 参数名 `sp` 很容易误导：它只是通过 x0 传进来的地址。函数中没有 `mov sp, ...`，当前调用者始终在自己的栈上执行。

## 4. 逐条解释 make 汇编

以下保留源码的有效指令，去掉 C++ 字符串包装和原注释：

```asm
bthread_make_fcontext:
    and x0, x0, ~0xF
    sub x0, x0, #0xb0
    str x2, [x0, #0xa0]
    adr x1, finish
    str x1, [x0, #0x98]
    ret x30
finish:
    mov x0, #0
    bl _exit
```

### 4.1 `and x0, x0, ~0xF`：地址向下对齐到 16 字节

`0xF` 的低四位为 1，取反后按位 AND，会把地址的低四位清零。设输入地址为 H，计算结果为：

```text
T = H & ~15
```

例如 `H = 0x10008`，对齐后 `T = 0x10000`，最多舍弃高端 15 字节。使用向下对齐，是为了不越过提供的栈内存上界。这里只修改 x0，不修改硬件 SP。

### 4.2 `sub x0, x0, #0xb0`：为初始上下文预留 176 字节

`0xb0 = 176`，于是：

```text
C = T - 176
```

C 就是函数将返回的 `bthread_fcontext_t`。176 是 16 的倍数，因此 C 仍然 16 字节对齐。这个空间位于目标栈内，并非另行 malloc 的对象。

为什么预留 176 字节，见下一节的布局。make 和 jump 必须对这个布局达成完全一致的约定。

### 4.3 `str x2, [x0, #0xa0]`：把入口地址写入恢复位置

向 `C + 160` 写入 8 字节函数指针 fn。以后 jump 会从这个位置加载跳转地址，再用 `ret x4` 转移到 fn。

源码称这个槽位为 PC，但这里保存的是一个普通内存中的代码地址；make 并没有改变 CPU 的 PC 去执行 fn。

### 4.4 `adr x1, finish`：计算 finish 标签的代码地址

`adr` 计算标签的 PC 相对地址，结果放入 x1。它不读取标签处的内存，也不调用 finish。

此时 `size` 原先占用的 x1 被覆盖；本分支没有做栈大小校验。调用者必须保证传入的栈区有效且足够大，不能指望此函数检查 guard 或栈溢出。

### 4.5 `str x1, [x0, #0x98]`：设置初始 LR

把 finish 地址写入 `C + 152`，这个位置对应恢复时的 x30。以后进入 fn 时：

```text
PC = fn
LR = finish
```

如果 fn 正常返回，它就会跳到 finish。注意 fn 的返回地址不是创建者调用 make 后的地址。

### 4.6 `ret x30`：make 本身正常返回

make 没有覆盖当前 x30，因此这里返回创建者；x0 仍为 C，就是 C++ 返回值。

我们写入的是目标栈保存区里的“将来恢复的 LR”，不是当前 CPU 中 make 用来返回的 LR。

### 4.7 finish：入口意外返回时的终止路径

```asm
mov x0, #0
bl _exit
```

把退出状态设为 0，再调用 `_exit(0)`。这是结束进程的库接口，不是结束一个 bthread，也不会替代调度器的任务清理。

业务函数 `m->fn` 返回，正常情况下只是回到 `task_runner`；runner 会做局部存储清理、完成通知和结束调度。不能把业务函数返回与最外层汇编入口 fn 返回混为一谈。

## 5. 两个函数共用的 176 字节布局

以 C 为保存区起始地址，表中的偏移都是字节。

| 偏移 | 大小 | jump 保存/恢复的内容 | make 首次构造时 |
|---|---:|---|---|
| `0x00 / 0x08` | 16 | d8 / d9 | 未初始化 |
| `0x10 / 0x18` | 16 | d10 / d11 | 未初始化 |
| `0x20 / 0x28` | 16 | d12 / d13 | 未初始化 |
| `0x30 / 0x38` | 16 | d14 / d15 | 未初始化 |
| `0x40 / 0x48` | 16 | x19 / x20 | 未初始化 |
| `0x50 / 0x58` | 16 | x21 / x22 | 未初始化 |
| `0x60 / 0x68` | 16 | x23 / x24 | 未初始化 |
| `0x70 / 0x78` | 16 | x25 / x26 | 未初始化 |
| `0x80 / 0x88` | 16 | x27 / x28 | 未初始化 |
| `0x90` | 8 | x29 / FP | 未初始化 |
| `0x98` | 8 | x30 / LR | finish 地址 |
| `0xa0` | 8 | 恢复执行地址 | fn 地址 |
| `0xa8` | 8 | 对齐填充 | 未使用 |

合计：64 字节浮点寄存器 + 96 字节通用寄存器 + 8 字节执行地址 + 8 字节填充 = 176 字节。

“未初始化”是指 make 没有写这些槽位，不能假设其中为零。首次入口没有需要恢复的历史局部变量；普通编译代码也不能假设传入的 x19 等寄存器有特定业务含义。这里同样没有主动构造一个 x29 为零的调试回溯链终点。

## 6. 一个可手算的首次启动例子

设目标栈已经分配，`H = T = 0x10000`：

```text
C = 0x10000 - 0xb0 = 0xff50
[0xff50 + 0x98] = [0xffe8] = finish
[0xff50 + 0xa0] = [0xfff0] = fn
make 返回 0xff50
```

后来调用 `jump(&old, C, 123)`。恢复阶段会：

```text
SP ← C
LR ← [C + 0x98] = finish
x0 ← 123
x4 ← [C + 0xa0] = fn
SP ← C + 0xb0 = 0x10000
ret x4 → fn(123)
```

这就完成了首次进入。fn 接收到的参数来自 **jump 的第三个参数**，而不是 make 的 size 参数。项目 `jump_stack` 传入 0，对应普通入口 `task_runner(0)` 的 `skip_remained`。

首次进入 fn 后，初始保存区已在当前 SP 以下，不再是必须永久保留的记录。fn 的正常栈帧可以覆盖它；以后挂起时，jump 会在当时的 SP 以下重新建立保存区，并更新 context 指针。

## 7. 汇编外层声明如何阅读

`context.cpp` 的 `__asm(...)` 是文件作用域汇编；相邻 C++ 字符串拼接后交给汇编器。外围这些不是运行时指令：

| 声明 | 含义 |
|---|---|
| `.cpu generic+fp+simd` | 选择支持浮点/SIMD 指令的目标配置 |
| `.text` | 后续内容进入代码段 |
| `.align 2` | 本 AArch64 GNU 汇编语境下按 2²，即 4 字节对齐代码 |
| `.global ...` | 导出符号供链接器使用 |
| `.type ..., %function` | 标记符号类型为函数 |
| `.size ...` | 描述符号的代码大小 |
| `.note.GNU-stack` | 声明该目标文件不需要可执行栈；与 guard 权限不同 |

## 8. 阅读检查与来源

读完应能解释：make 为什么不改变当前 SP；为什么返回地址 C 比栈高端低 176 字节；为什么 `0x98` 与 `0xa0` 写入不同地址；为什么业务 fn 返回不等于执行 `_exit`。

项目依据：[context.cpp](../../src-bthread/context.cpp)、[context.h](../../src-bthread/context.h)、[stack_inl.h](../../src-bthread/stack_inl.h)、[stack.cpp](../../src-bthread/stack.cpp)、[task_group.cpp](../../src-bthread/task_group.cpp)。

ABI 基础依据：[Arm 官方 AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)，重点阅读寄存器、栈约束、子程序调用。本文说明当前源码实现的基础上下文格式，不把它扩展解释为覆盖所有 ARM64 扩展状态的通用保存器。
