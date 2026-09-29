# VersionedRefWithId：版本化引用与回收竞争

> 源码：[versioned_ref_with_id.h](../../inc/versioned_ref_with_id.h)，移植自 brpc `brpc/versioned_ref_with_id.h`。依赖 `butil/resource_pool.h`（`ResourceId`/`get_resource`/`address_resource`/`return_resource`）。

## 用途

用一个 64 位 `VRefId` 在 O(1) 时间内定位一个引用计数对象（CRTP 基类），并用一个原子字 `_versioned_ref` 同时维护**版本**与**引用计数**。它解决两件事：

1. 用 `VRefId` 安全寻址对象，防止用陈旧的 id 访问已回收对象（版本号校验）。
2. 对象在最后一次解引用时自动回收，且恰好回收一次（版本号做唯一回收仲裁）。

`Create()` 返回 `VRefId` 给外部保存；外部每次访问都通过 `Address(id)` 得到 `VersionedRefWithIdUniquePtr<T>`，`unique_ptr` 析构时自动减引用。

## 编码与状态

```
_versioned_ref = [ version (32-bit 无符号) | nref (32-bit 有符号引用计数) ]
VRefId         = [ version (32-bit 无符号) | slot (32-bit 池槽下标) ]
```

两者的低 32 位含义不同：`_versioned_ref` 低 32 位是 nref（多少个持有者），`VRefId` 低 32 位是 slot（哪个对象）。它们靠**高 32 位的 version 相互校验**。

version 全程按 +1 递增，编码对象一生的三个状态：

```
id_ver      (偶数)  已创建/存活   Address(id) 命中
id_ver + 1  (奇数)  已 failed     SetFailed 成功时 CAS 上去
id_ver + 2  (偶数)  已回收       最后一次 Dereference 时 CAS 上去
```

奇数= failed、偶数= alive 或 recycled，`Address()` 与 `AddressFailedAsWell()` 靠这个奇偶区分三个状态。

关键事实：**版本跨化身递增、不归零**。复用 slot 时 `get_resource` 返回 `unsafe_address_resource(free_id)` 而不重跑构造函数，所以 `_versioned_ref` 不清零；一次完整「回收 + 重建」使版本 +2。这是陈旧 id 能被识别的基础。

## 额外引用

`Create()` 里有一次 `fetch_add(1)`，让 nref 从 0 到 1。这个 +1 是对象「自持」的额外引用，保证刚创建、还没人 `Address()` 的对象不被立即回收。

释放额外引用有两条路径，最终都汇到 `Dereference()`：

| 路径 | version 变化 | 最终 ver |
|------|--------------|----------|
| `SetFailed()` → 内部 `ReleaseAdditionalReference()` | id_ver → id_ver+1 | id_ver+1（奇数） |
| 直接 `ReleaseAdditionalReference()`（干净关闭） | 不变 | id_ver（偶数） |

`ReleaseAdditionalReference()` 是公开方法，可独立调用；`SetFailed` 只是先 version+1 再通知、再调用它。`_additional_ref_status` 的 `USING/REVIVING/RECYCLED` 三态机负责在 `SetFailed` 与 `Revive` 并发时保证额外引用只释放/加回一次。

## Dereference 的「No retry」：两个回收者

`Dereference()` 在 nref 从 1 减到 0 时，尝试 `CAS` 把 version 置成 `id_ver+2` 并 `return_resource`。但这个 CAS **不重试**，因为可能存在两个并发「回收者」：

- **SetFailed 的 Dereference**：`SetFailed → ReleaseAdditionalReference → Dereference`，释放额外引用。
- **陈旧 Address 的 AddressImpl**：`fetch_add` 发现版本不匹配，再 `fetch_sub` 撤销，也可能把 nref 减到 0。

两条路径都可能看到「nref 1→0」，都尝试回收。CAS 保证只有一个成功。设初始 `(version=id_ver+1, nref=1)`（已 failed，只剩额外引用），记：

```
S1 = SetFailed 的 fetch_sub(1)    // --nref，看到 1→0
S2 = SetFailed 的 CAS → id_ver+2   // 尝试回收
A3 = Address 的 fetch_add(1)       // ++nref，版本不匹配
A4 = Address 的 fetch_sub(1)       // --nref，看到 1→0
A5 = Address 的 CAS → id_ver+2     // 尝试回收
```

三种交错（`(version, nref)` 记法）：

**① `1,2,3,4,5` 或 `1,3,4,2,5` → SetFailed 成功，Address 失败**

```
初始        (id_ver+1, 1)
S1          (id_ver+1, 0)   SetFailed 看到 1→0
A3          (id_ver+1, 1)   nref 又变 1
A4          (id_ver+1, 0)   Address 看到 1→0
S2          CAS 成功 → (id_ver+2, 0)   SetFailed 回收
A5          CAS 期望 (id_ver+1,0)，当前已 (id_ver+2,0) → 失败
```

`1,2,3,4,5` 同理：S2 先成功，之后 A3 读到偶数 `id_ver+2`（已回收），A4 后走「Addressed a free slot」分支，也不回收。

**② `1,3,2,4,5` → SetFailed 失败，Address 回收**

```
初始        (id_ver+1, 1)
S1          (id_ver+1, 0)
A3          (id_ver+1, 1)   nref 被顶回 1
S2          CAS 期望 (id_ver+1,0)，当前 (id_ver+1,1) → nref 变了，失败
A4          (id_ver+1, 0)
A5          CAS 成功 → (id_ver+2, 0)   Address 回收
```

**③ `1,3,4,5,2` → SetFailed 失败，槽位已被 Address 提前回收**

```
初始        (id_ver+1, 1)
S1          (id_ver+1, 0)
A3          (id_ver+1, 1)
A4          (id_ver+1, 0)
A5          CAS 成功 → (id_ver+2, 0)   Address 先回收
S2          CAS 期望 (id_ver+1,0)，当前已 (id_ver+2,0) → version 变了，失败
```

**「No retry」的两条原因**（对应注释原文）：

- version 变了 → 别人已经回收，重试就是双重回收。
- nref 变了 → 又有引用进来（nref 非零），等下次谁把 nref 1→0 时自然由那个人回收。

所以这次 CAS 是**单次原子「认领」**：谁成功谁回收，失败则要么「已回收」要么「留给后来者」，都不该也不能重试。无论顺序如何，对象恰好回收一次。

## AddressImpl 的 `ver1 + 1 == ver2`：兜底回收

`AddressImpl` 在版本不匹配后撤销自己的 +1（`fetch_sub`），若此时 nref 归零，它也可能成为最后一个回收者。其中回收分支的条件：

```cpp
const uint32_t ver1 = VersionOfVRef(vref1);   // fetch_add 时读到的版本
const uint32_t ver2 = VersionOfVRef(vref2);   // fetch_sub 时读到的版本
if ((ver2 & 1)) {                              // ver2 奇数 = failed 态
    if (ver1 == ver2 || ver1 + 1 == ver2) { ... 回收 ... }
}
```

`ver1 == ver2` 是「两个原子操作之间没人动版本」；`ver1 + 1 == ver2` 是「中间被并发 SetFailed 抢跑了一次 +1」。后者需要三个前提同时成立：

1. **陈旧 id**：`ver1 != VersionOfVRefId(id)`，否则第一道检查就 `return 0`。版本跨化身递增，旧化身的 id 版本（如 0）对不上当前 alive 版本（如 2）。
2. **nref 基线为 0**：对象额外引用已被释放，A 的 `fetch_add` 才把 nref 变 1、`fetch_sub` 才返回 `nref == 1`（A 是最后一个）。
3. **并发 SetFailed 抢跑 +1**：落在 A 的 fetch_add 与 fetch_sub 之间，把版本从偶数 alive 打成奇数 failed。

具体场景（记 `(version, nref)`，对象化身2 alive 版本 = 2，A 持旧 id 版本 = 0）：

```
B = 干净关闭线程（ReleaseAdditionalReference）
A = 陈旧 Address 线程（持旧 id）
C = 并发 SetFailed 线程

初始        (2, 1)    // 化身2 alive，只剩额外引用

B1  释放额外引用 → fetch_sub → (2, 0)   // B 看到 1→0，但 recycle CAS 还没做（limbo）
A1  fetch_add → (2, 1)                 // A 读到 ver1=2（偶数）；id=0 ≠ 2 → 落空
C1  SetFailed → CAS 2→3 → (3, 1)       // 版本 +1（alive→failed）
A2  fetch_sub → (3, 0)                 // A 读到 ver2=3（奇数），nref=1
    // nref==1、ver2 奇数、ver1+1==ver2 (2+1==3) → A 兜底回收
```

本质：B 释放了额外引用，但其 recycle CAS 被 A 的 `fetch_add` 抢跑顶掉而丢失；A 成为最后一个引用后，兜底完成本该由 B 做的回收。

## 容易误解的地方

- `ver1 == id_ver` 命中 `Address` 需要 `VersionOfVRefId(id)` 等于对象当前 alive 版本。陈旧 id 来自上一化身，版本差 2，所以「对象明明 alive（偶数）却对不上」。
- `Dereference` 注释里的 `ver == id_ver` 是「干净回收」（直接 `ReleaseAdditionalReference`，version 停在偶数），`ver == id_ver+1` 是「先 SetFailed」（version 被 +1 成奇数），两种情况都要销毁，不是「正常 vs 异常」的简单二分。
- `ver1 + 1 == ver2` 不是另一条业务分支，而是「两个原子操作之间版本恰好前进了一步」的合法竞态；前进 ≥2 步或倒退才 `CHECK(false)`。
- 额外引用不是只有 `SetFailed` 才能释放，`ReleaseAdditionalReference()` 本身是公开的独立释放入口。

## 自测

1. 为什么 `_versioned_ref` 的版本要在 fail 和 recycle 各 +1（而不是只 +1）？
2. `Dereference` 的 recycle CAS 为什么「No retry」？version 变和 nref 变分别意味着什么？
3. 陈旧 `Address` 为什么可能走到 `nref == 1` 分支？它和 `SetFailed` 的 `Dereference` 谁是回收者由什么决定？
4. `ver1 + 1 == ver2` 需要哪三个前提同时成立？为什么对象「alive（偶数）却对不上 id」只能由陈旧 id 解释？
