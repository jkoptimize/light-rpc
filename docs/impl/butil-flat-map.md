# butil/flat_map — 首节点内联的分离链哈希表

> 本文记录 flat_map 的数据结构定位、相比 `std::unordered_map` 的性能优势，以及 `operator=` / `erase` 两个实现里的设计权衡。移植裁剪决策见 `src-bthread/butil/flat_map.h` 顶部注释（裁掉了 Sparse/Multi/FlatSet 等分支）。

## 1. 数据结构：闭合寻址 + 首节点内联

flat_map 是**闭合寻址（分离链）**，不是开放寻址。关键设计是「**首节点内联在桶数组里**」（flat_map.h:145-180）：

```cpp
struct Bucket {
    Bucket* next;                              // 冲突链指针
    ManualConstructor<Element> element_space_; // key+value 内联在 bucket 里
};
// Element（flat_map.h:237-270）
class FlatMapElement { K _key; T _value; };
```

- 桶数组 `Bucket* _buckets` 指向**连续的 Bucket 数组**，每个 bucket 内联 `next + key + value`。
- 只有哈希冲突的**额外节点**才从 `SingleThreadedPool` 分配，通过 `next` 链起来。
- 小 map 优化：`_default_buckets[DEFAULT_NBUCKET+1]` 内嵌 16 个 bucket，小规模不堆分配。

## 2. 相比 std::unordered_map 的性能优势

两者的寻址路径「指针 → 数组 → 偏移」看似相同，**关键区别在「偏移之后拿到的是什么」**：

| | std::unordered_map | flat_map |
|---|---|---|
| 桶数组里存什么 | 指向堆 node 的**指针** | **内联的 key+value** |
| 命中时内存访问 | **2 次**（读桶指针 + 解引用散落 node） | **1 次**（直接读连续数组里的 bucket） |
| 第二次访问地址 | 堆上散落，大概率 cache miss | 连续数组，cache 友好 |

查找命中路径对比（flat_map_inl.h:383-390）：

```cpp
// flat_map：直接索引到内联元素
Bucket& first_node = _buckets[flatmap_mod(_hashfn(key), _nbucket)];
if (_eql(first_node.element().first_ref(), key)) { ... }

// unordered_map：桶指针 → 再解引用散落 node
node* n = bucket_array[hash(key) % count];
if (n->key == key) { ... }
```

「`_buckets` 指针寻址到数组」这一步两边都几乎零成本（成员在 `this` 附近的 cache line），真正的差异全在「偏移之后」：flat_map 一条 `mov` 就拿到 key，unordered_map 还要再解引用一次散落的 node。

**附带收益**：① 首节点不 `new`（内联在数组），无堆分配碎片；② 内存占用更小（省掉桶指针数组 + node 堆头）；③ 连续数组 CPU 预取友好。

**边界**：flat_map 的冲突链（`p = p->next`）仍是间接访问，与 unordered_map 的链表无本质区别。它的假设是「冲突是少数、首节点命中是常见路径」，用高质量哈希（fmix64）+ 负载因子 resize 保证这一点。

## 3. operator= 的两条路径：结构级复制 vs 语义级重建

`operator=`（flat_map_inl.h:214-236）按 `_nbucket` 是否相等分两路：

```cpp
if (_nbucket == rhs._nbucket) {
    // 结构级复制：直接按索引 i placement new 拷贝
    for (size_t i = 0; i < rhs._nbucket; ++i)
        if (rhs._buckets[i].is_valid())
            new (&_buckets[i]) Bucket(rhs._buckets[i]);
} else {
    // 语义级重建：逐元素重新哈希插入
    for (auto it = rhs.begin(); it != rhs.end(); ++it)
        operator[](first(*it)) = second(*it);
}
```

结构级复制能成立的前提是 **`_nbucket` 相等 → 哈希映射一致 → 同一 key 落同一 bucket index**。性能差异分三层（从核心到次要）：

1. **省掉 N 次哈希**（核心）：结构级复制零次 `_hashfn`；语义级重建每个元素 `operator[]` 算一次哈希。
2. **省掉 N 次查找 + 插入**：结构级复制零次 `_eql`/`seek`/`is_too_crowded`/resize。
3. **省掉 iterator 分支遍历**（次要）：`for (i=0; i<nbucket; ++i)` 纯整数循环，vs iterator 的 `operator++` 每次判断 `_node->next` 并 `find_and_set_valid_node()` 跳过 invalid bucket。

## 4. erase 的正确性：memcpy vs operator=

删除**首节点且带冲突链**时，首节点内联无法释放，必须把 next 链第一个节点「搬进」首节点（flat_map_inl.h:307-328）。直观方案是 memcpy：

```cpp
first_node.destroy_element();
first_node = *p;   // 看似正确，实际是 trivial copy（memcpy）
```

**为什么错**：`first_node = *p` 触发 Bucket 默认拷贝赋值 → `element_space_`（AlignedMemory，POD）是 trivial copy（memcpy），**绕过 value 类型的赋值语义**。对于自引用类型：

```cpp
Value { Value() : num(0), num_ptr(&num) {}  int num;  int* num_ptr; };
```

memcpy 后 `num_ptr` 还指向 `p` 内部，而 `p` 随后被销毁 → 悬空指针。

正确做法是显式调用 key/value 的 `operator=`（flat_map_inl.h:323-325）：

```cpp
first_ref() = p->first_ref();                 // K::operator=
second_ref() = p->second_movable_ref();       // T::operator= (move)
```

**「Calling operator= is the price that we have to pay」的准确含义**：flat_map 放弃了最朴素的 memcpy，改而调用 `operator=`，把「如何正确复制」的责任**交给 value 类型自己**。这个「price」不是性能损失——对平凡类型 operator= 与 memcpy 一样快（编译器生成相同机器码）；对非平凡类型，operator= 慢的部分是正确性（如重算自引用指针）必须做的工作。真正的代价是「**正确性从 flat_map 转移到了类型身上**」：flat_map 不再假设 value 可 memcpy，而是尊重类型的赋值语义。
