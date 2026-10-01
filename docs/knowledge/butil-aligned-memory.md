# butil/aligned_memory.h — AlignedMemory 对齐存储单元

## 1. 作用概述

`AlignedMemory<Size, ByteAlignment>` 是一个 **POD 类型模板**，用于在栈上或静态区声明一段**指定大小 + 指定对齐**的原始字节存储，并让调用方**手动控制对象的构造与析构时机**。

它的核心价值是：把「内存的分配」与「对象的构造」分离。普通的 `T obj` 会在声明处立即执行构造函数，在离开作用域时立即析构；而 `AlignedMemory` 只是占一块对齐好的裸字节，对象何时 `new`（placement new）进去、何时手动调用析构，完全由调用方决定。

```cpp
static AlignedMemory<sizeof(T), alignof(T)> slot;   // 只占内存，不构造 T
new (slot.void_data()) T(args...);                  // 手动构造
T* p = slot.data_as<T>();                           // 按 T 访问
p->~T();                                            // 手动析构
```

## 2. 关键 API

| 符号 | 签名 | 说明 |
|------|------|------|
| `AlignedMemory<Size, Align>` | 模板类（主模板未定义，13 个特化） | 对齐裸存储，`Size` 字节、`Align` 对齐 |
| `void_data()` | `void*` / `const void*` | 返回存储起始地址的 `void*` |
| `data_as<T>()` | `T*` / `const T*` | 把存储按类型 `T` 重新解释 |
| 特化列表 | `Align = 1,2,4,8,16,32,64,128,256,512,1024,2048,4096` | 显式列出支持的 13 种对齐 |

实现要点：
- 存储成员是 `alignas(byte_alignment) uint8_t data_[Size]`，靠 C++11 `alignas` 关键字保证对齐，不依赖编译器扩展。
- 私有的 `operator new` / `operator delete` 声明**禁止堆上动态分配** `AlignedMemory` 对象本身（它本意就是栈/静态存储，防止被误 `new`）。

## 3. 在 bthread 中的作用

`AlignedMemory` 是 **`resource_pool` 和 `object_pool` 的底层存储单元**，而这两个池是 bthread 调度器的内存基础。

- `resource_pool_inl.h`：`typedef AlignedMemory<sizeof(T), __alignof__(T)> BlockItem;`
  - `BlockItem` 是一个 Block 里内嵌的单个 `T` 槽位。`resource_pool` 用它做 ABA-free 的 ID→对象映射：每个 `ResourceId` 指向某个 Block 里的某个 `BlockItem`。
  - 承载的对象是 bthread 调度核心的 **`TaskMeta`**（task_meta.h）和 **`TimerTask`**，两者都是高频分配/释放、且对性能极敏感的对象。
- `object_pool_inl.h`：`typedef AlignedMemory<sizeof(T), __alignof__(T) < 8 ? 8 : __alignof__(T)> BlockItem;`
  - 承载 `ContextualStack`（协程上下文栈）和 `ButexWaiter`（butex 等待节点）。

**为什么对齐在这里是硬需求**：`TaskMeta`、`ContextualStack` 等结构体内部含 `BAIDU_CACHELINE_ALIGNMENT` 成员（64 字节对齐）或原子字段。若存储单元不对齐，会产生 cache-line 假共享、未对齐访问甚至 UB。`AlignedMemory` 以 `__alignof__(T)` 精确对齐，保证了池中每个对象都满足其类型的对齐要求。

## 4. 移植裁剪决策

| 原文件内容 | 决策 | 理由 |
|------------|------|------|
| `AlignedAlloc(size, alignment)` | **删除** | 运行时对齐分配，仅被 `AlignedFreeDeleter`/`scoped_ptr` 使用，bthread 核心不调用 |
| `AlignedFree(ptr)` | **删除** | 同上 |
| `AlignedFreeDeleter` | **删除** | 依赖 `scoped_ptr`，不移植 |
| `#include "butil/base_export.h"`（`BUTIL_EXPORT`） | **删除** | 静态库无需导出符号 |
| `#include "butil/basictypes.h"`（`uint8_t`） | **替换为 `<stdint.h>`** | basictypes.h 不移植 |
| `#include "butil/compiler_specific.h"`（`ALIGNAS`） | **删除** | C++17 直接用 `alignas` 关键字 |
| `DECL_ALIGNED_BUFFER` 宏（区分 C++11 前后） | **内联为 `alignas`** | 项目是 C++17，恒走 `alignas` 分支 |
| MSVC 的 `#include <malloc.h>` | **删除** | build_config.h 保证仅 GCC/Clang |

## 5. 与 std 的关系

C++ 标准库有 `std::aligned_storage<Size, Align>` 提供几乎相同的功能，**但它已在 C++23 被标记 deprecated**（理由见 [P1413R3](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/p1413r3.pdf)），且 `std::aligned_storage` 的 `::type` 用法冗长、对 `Align` 为常量表达式的约束更弱。

本移植保留 `AlignedMemory` 而非替换为 `std::aligned_storage` 的理由：

1. **对齐参数是编译期常量**：`__alignof__(T)` 直接作为模板参数，`alignas` 成员比 `aligned_storage` 的 `::type` 更直观。
2. **与 resource_pool/object_pool 的 `BlockItem` typedef 一一对应**：这两个池的源码直接 `typedef AlignedMemory<...> BlockItem`，保留原名最小化移植改动。
3. **语义清晰**：`AlignedMemory` 是纯 POD 存储、零运行时开销，`data_as<T>()`/`void_data()` 的显式转换接口比 `std::launder`/`reinterpret_cast` 更符合池代码的既有写法。
