# Lab2 索引管理：完整学习与实现指南

> 这份文档的目标：让你**完全理解** Lab2 的每一个 TODO，然后**自己动手**把代码写进源文件。
>
> 文档结构：先教你原理 → 再告诉你该改哪个文件的哪几行 → 给你可直接复制的完整代码 → 最后给你验证方法。

---

## 目录

1. [你需要先搞懂的基础知识](#1-你需要先搞懂的基础知识)
2. [IxNodeHandle 节点内操作：8 个 TODO](#2-ixnodehandle-节点内操作8-个-todo)
3. [IxIndexHandle 查找操作：2 个 TODO](#3-ixindexhandle-查找操作2-个-todo)
4. [IxIndexHandle 插入操作：3 个 TODO](#4-ixindexhandle-插入操作3-个-todo)
5. [IxIndexHandle 删除操作：5 个 TODO](#5-ixindexhandle-删除操作5-个-todo)
6. [IxIndexHandle 范围查找：2 个 TODO](#6-ixindexhandle-范围查找2-个-todo)
7. [SmManager DDL：2 个 TODO](#7-smmanager-ddl2-个-todo)
8. [B+ 树并发控制：3 个 TODO](#8-b树并发控制3-个-todo)
9. [完整测试流程](#9-完整测试流程)
10. [调试技巧](#10-调试技巧)
11. [附录：各函数速查表](#附录各函数速查表)

---

## 1. 你需要先搞懂的基础知识

### 1.1 B+ 树基本概念

本项目中的 B+ 树有以下特点：

```
              [10 | 20]                    ← 内部节点（只存 key + 子节点指针）
             /    |    \
        [5|8]  [10|15]  [20|25|30]        ← 内部节点
        / | \   / | \    / | | \
      叶子节点通过 prev/next 形成双向链表   ← 叶子节点（存 key + Rid）
```

关键规则：
- **叶子节点**存 `(key, Rid)` 对，Rid 指向记录在磁盘上的位置
- **内部节点**存 `(key, child_page_no)` 对，child_page_no 指向子节点
- 每个节点的**第一个 key** 存储的是以该节点为根的子树中所有 key 的最小值
- 叶子节点通过 `prev_leaf` / `next_leaf` 形成**双向链表**，方便范围扫描
- **不支持重复键**（唯一索引）
- 每个节点最多存 `btree_order` 个键值对，最少存 `btree_order / 2` 个（根节点特殊，最少 1 个或 2 个）

### 1.2 索引文件页面布局

```
Page 0: IxFileHdr（文件头，存储元信息）
Page 1: IxPageHdr（叶子链表头节点，is_leaf=true，不存数据，只做哨兵）
Page 2: IxPageHdr（根节点，初始为空叶子节点）
Page 3+: 数据节点
```

- `IX_FILE_HDR_PAGE = 0`：文件头页
- `IX_LEAF_HEADER_PAGE = 1`：叶子链表头（哨兵节点）
- `IX_INIT_ROOT_PAGE = 2`：初始根节点
- `IX_INIT_NUM_PAGES = 3`：初始页面数

### 1.3 IxFileHdr 关键字段

```cpp
class IxFileHdr {
    page_id_t first_free_page_no_;  // 空闲页链表头
    int num_pages_;                 // 文件总页数
    page_id_t root_page_;           // 根节点页号（初始为 2）
    int col_num_;                   // 索引字段数量
    std::vector<ColType> col_types_; // 字段类型
    std::vector<int> col_lens_;     // 字段长度
    int col_tot_len_;               // 字段总长度
    int btree_order_;               // 每个节点最多键值对数
    int keys_size_;                 // = (btree_order + 1) * col_tot_len
    page_id_t first_leaf_;          // 叶子链表第一个叶子
    page_id_t last_leaf_;           // 叶子链表最后一个叶子
};
```

**btree_order 的计算公式**：
```
|page_hdr| + (|attr| + |rid|) * (n + 1) <= PAGE_SIZE
btree_order = (PAGE_SIZE - sizeof(IxPageHdr)) / (col_tot_len + sizeof(Rid)) - 1
```

为什么要减 1：多留一个空位方便分裂操作。

### 1.4 IxPageHdr 关键字段

```cpp
class IxPageHdr {
    page_id_t next_free_page_no;  // 空闲页链表
    page_id_t parent;             // 父节点页号
    int num_key;                  // 当前键值对数量
    bool is_leaf;                 // 是否叶子节点
    page_id_t prev_leaf;          // 前一个叶子（仅叶子节点有效）
    page_id_t next_leaf;          // 后一个叶子（仅叶子节点有效）
};
```

### 1.5 IxNodeHandle 内存布局

每个节点在 Page 中的内存布局：

```
[IxPageHdr][keys 数组（长度 = keys_size_）][rids 数组]
```

- `keys` 数组：每个 key 占 `col_tot_len` 字节，连续存储
- `rids` 数组：每个 `Rid` 占 `sizeof(Rid)` 字节，连续存储
- `get_key(i)` 返回 `keys + i * col_tot_len`（第 i 个 key 的首地址）
- `get_rid(i)` 返回 `&rids[i]`（第 i 个 Rid 的指针）

图解：
```
keys:  [key0|key1|key2|...]
       |    |    |
       col_tot_len 字节每个

rids:  [rid0|rid1|rid2|...]
       |    |    |
       sizeof(Rid) 字节每个
```

### 1.6 ix_compare 函数

用于比较两个 key 的大小，已提供在 `ix_index_handle.h` 中：

```cpp
// 单字段比较
int ix_compare(const char *a, const char *b, ColType type, int col_len);
// 返回值：-1(a < b)，0(a == b)，1(a > b)

// 多字段比较（联合索引）
int ix_compare(const char* a, const char* b, const std::vector<ColType>& col_types, const std::vector<int>& col_lens);
```

### 1.7 辅助函数说明（已提供，无需实现）

这些函数在 `IxIndexHandle` 类中已经实现，你可以直接调用：

| 函数 | 作用 | 注意事项 |
|------|------|----------|
| `fetch_node(page_no)` | 获取指定页号的 IxNodeHandle | **会 pin page，记得 unpin** |
| `create_node()` | 创建新节点 | 会 pin page，记得 unpin |
| `maintain_parent(node)` | 从 node 向上更新父节点的第一个 key | 直到根节点 |
| `maintain_child(node, child_idx)` | 设置 node 的第 child_idx 个子节点的 parent | 用于内部节点 |
| `erase_leaf(leaf)` | 删除叶子前更新前后指针 | 调用前叶子必须是叶子节点 |
| `release_node_handle(node)` | 删除节点后更新 file_hdr 的 num_pages | 不会 unpin |
| `find_child(child)` | 在 parent 中查找 child 的 rid_idx | 返回 child 在 parent 中的位置 |

---

## 2. IxNodeHandle 节点内操作：8 个 TODO

**文件**：`src/index/ix_index_handle.cpp`

这 8 个函数都是在**单个节点内部**操作，不涉及树的遍历。它们是后续所有树操作的基础。

---

### TODO 2.1：lower_bound

**位置**：`ix_index_handle.cpp` 第 21-27 行

**做什么**：在当前节点的 keys 数组中，找到第一个 **>= target** 的 key 的位置。

**你需要理解的**：
- 这是一个标准的二分查找，但 key 的长度是 `col_tot_len` 而不是固定 4 字节
- 返回值范围 `[0, num_key]`，如果返回 `num_key` 表示 target 大于所有 key
- 返回的 index 同时也是 rid 数组的 index（key 和 rid 一一对应）

**直接复制到源文件的代码**：
```cpp
int IxNodeHandle::lower_bound(const char *target) const {
    // 二分查找：在当前节点的 keys 数组中找第一个 >= target 的位置
    int left = 0, right = get_size();
    while (left < right) {
        int mid = left + (right - left) / 2;
        // 用 ix_compare 比较 mid 位置的 key 和 target
        // 注意：这里用的是单字段比较，col_types_[0] 和 col_tot_len_
        if (ix_compare(get_key(mid), target, file_hdr->col_types_[0], file_hdr->col_tot_len_) < 0) {
            left = mid + 1;  // mid 的 key < target，目标在右半边
        } else {
            right = mid;     // mid 的 key >= target，目标在左半边（含 mid）
        }
    }
    return left;
}
```

**为什么这么写**：
- 使用 `left + (right - left) / 2` 而非 `(left + right) / 2` 防止整数溢出
- `ix_compare` 返回 -1/0/1，用 `< 0` 判断 "小于"
- 当 `left == right` 时循环结束，此时 `left` 就是第一个 >= target 的位置

---

### TODO 2.2：upper_bound

**位置**：`ix_index_handle.cpp` 第 35-41 行

**做什么**：在当前节点的 keys 数组中，找到第一个 **> target** 的 key 的位置。

**你需要理解的**：
- 和 `lower_bound` 几乎一样，唯一区别是 `ix_compare` 的比较条件
- 返回值范围 `[1, num_key]`（注意从 1 开始）

**直接复制到源文件的代码**：
```cpp
int IxNodeHandle::upper_bound(const char *target) const {
    // 二分查找：找第一个 > target 的位置
    int left = 0, right = get_size();
    while (left < right) {
        int mid = left + (right - left) / 2;
        // 和 lower_bound 的唯一区别：这里用 <= 0 而非 < 0
        // <= 0 表示 mid 的 key <= target，目标在右半边
        if (ix_compare(get_key(mid), target, file_hdr->col_types_[0], file_hdr->col_tot_len_) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}
```

**lower_bound vs upper_bound 的区别**：
- `lower_bound`：找 `>=`，用 `< 0` 跳过小于的
- `upper_bound`：找 `>`，用 `<= 0` 跳过小于等于的

---

### TODO 2.3：leaf_lookup

**位置**：`ix_index_handle.cpp` 第 51-59 行

**做什么**：在叶子节点中按键查找，找到则通过 `value` 指针返回对应的 Rid。

**你需要理解的**：
- 叶子节点的 key 和 rid 是一一对应的
- 用 `lower_bound` 找到候选位置，再检查 key 是否真的相等

**直接复制到源文件的代码**：
```cpp
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    // 1. 用 lower_bound 找到第一个 >= key 的位置
    int idx = lower_bound(key);
    // 2. 如果 idx 在范围内，且该位置的 key 等于 target，说明找到了
    if (idx < get_size() && ix_compare(get_key(idx), key, file_hdr->col_types_[0], file_hdr->col_tot_len_) == 0) {
        *value = get_rid(idx);  // 通过传出参数返回 Rid 指针
        return true;
    }
    return false;  // 没找到
}
```

---

### TODO 2.4：internal_lookup

**位置**：`ix_index_handle.cpp` 第 66-73 行

**做什么**：在内部节点中，根据 key 找到应该走哪个子节点。

**你需要理解的**：
- 内部节点的第 i 个 key 表示：其第 i 个子树中所有键 **>= key[i]**
- 所以用 `upper_bound` 找到第一个 > key 的位置 `idx`，key 应该在 `idx - 1` 号子树中

**直接复制到源文件的代码**：
```cpp
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    // upper_bound 返回第一个 > key 的位置
    int idx = upper_bound(key);
    // upper_bound 可能返回0（当target小于所有key时），此时应返回第一个孩子节点
    if (idx == 0) return value_at(0);
    // idx - 1 就是最后一个 <= key 的位置，其对应的子节点包含 key
    return value_at(idx - 1);
}
```

**为什么是 upper_bound + idx-1，以及 idx==0 的处理**：

根据 RucBase 文档："在每个内部结点的第一个值前面额外加上第一个键"，所以内部节点的实际布局是：

```
rid[0]  key[0]  rid[1]  key[1]  rid[2]  key[2]  ...
  ↑       ↑       ↑       ↑       ↑
第一个   第一个   第二个   分隔键   第三个
子节点   子节点   子节点           子节点
         最小key
```

- `key[0]` 是额外加的，存储第一个子节点中所有 key 的最小值
- `key[i]` (i>0) 是分隔键，`rid[i]` 指向 key[i] 右边的子节点（键 >= key[i]）
- `rid[0]` 指向第一个子节点（键 < key[1]）

查找逻辑：
- `upper_bound(target)` 返回第一个 > target 的位置 idx
- 当 `idx > 0`：target 在 key[idx-1] 和 key[idx] 之间，属于子节点 `rid[idx-1]`
- 当 `idx == 0`：target < 所有 key，属于第一个子节点 `rid[0]`（不处理会 `value_at(-1)` 段错误）

示例：key=[min, 10, 15]，查找 key=7：
- `upper_bound(7)` = 1（key[1]=10 > 7）
- `value_at(1-1) = value_at(0)` → 第一个子节点（键 < 10），正确！

---

### TODO 2.5：insert_pairs

**位置**：`ix_index_handle.cpp` 第 89-96 行

**做什么**：在节点的指定位置 `pos` 插入 `n` 个连续的键值对。

**你需要理解的**：
- keys 和 rids 是两个独立的数组，需要分别移动
- 先移动腾出空间，再复制新数据进去
- `memmove` 处理重叠区域（源和目标可能重叠），`memcpy` 不处理重叠

**直接复制到源文件的代码**：
```cpp
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    // 1. 移动 keys 数组：把 [pos, num_key) 的数据移到 [pos+n, num_key+n)
    //    腾出 [pos, pos+n) 的空间给新 key
    memmove(get_key(pos + n), get_key(pos), (page_hdr->num_key - pos) * file_hdr->col_tot_len_);
    // 2. 把新的 n 个 key 复制到 pos 位置
    memcpy(get_key(pos), key, n * file_hdr->col_tot_len_);
    // 3. 移动 rids 数组：和 keys 同理
    memmove(get_rid(pos + n), get_rid(pos), (page_hdr->num_key - pos) * sizeof(Rid));
    // 4. 把新的 n 个 rid 复制到 pos 位置
    memcpy(get_rid(pos), rid, n * sizeof(Rid));
    // 5. 更新键值对数量
    page_hdr->num_key += n;
}
```

**为什么用 memmove 而不是 memcpy**：当源地址和目标地址重叠时（比如把数据往后移），`memcpy` 的行为是未定义的，`memmove` 能正确处理。

---

### TODO 2.6：insert

**位置**：`ix_index_handle.cpp` 第 105-113 行

**做什么**：在节点中插入单个键值对，返回插入后的键值对数量。

**你需要理解的**：
- 先用 `lower_bound` 找到插入位置
- 如果 key 已存在则不插入（不支持重复键）
- 调用 `insert_pairs` 完成实际插入

**直接复制到源文件的代码**：
```cpp
int IxNodeHandle::insert(const char *key, const Rid &value) {
    // 1. 找到应该插入的位置（第一个 >= key 的位置）
    int pos = lower_bound(key);
    // 2. 检查 key 是否已存在
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_[0], file_hdr->col_tot_len_) == 0) {
        return get_size();  // 重复 key，不插入，返回当前大小
    }
    // 3. 在 pos 位置插入 1 个键值对
    insert_pairs(pos, key, &value, 1);
    return get_size();
}
```

---

### TODO 2.7：erase_pair

**位置**：`ix_index_handle.cpp` 第 120-126 行

**做什么**：删除节点中指定位置的键值对。

**你需要理解的**：
- 删除后需要把后面的数据往前移，填补空位
- keys 和 rids 分别处理

**直接复制到源文件的代码**：
```cpp
void IxNodeHandle::erase_pair(int pos) {
    // 1. 移动 keys：把 [pos+1, num_key) 的数据移到 [pos, num_key-1)
    //    覆盖掉 pos 位置的 key
    memmove(get_key(pos), get_key(pos + 1), (page_hdr->num_key - pos - 1) * file_hdr->col_tot_len_);
    // 2. 移动 rids：同理
    memmove(get_rid(pos), get_rid(pos + 1), (page_hdr->num_key - pos - 1) * sizeof(Rid));
    // 3. 键值对数量 -1
    page_hdr->num_key--;
}
```

---

### TODO 2.8：remove

**位置**：`ix_index_handle.cpp` 第 134-141 行

**做什么**：删除节点中指定 key 的键值对，返回删除后的数量。

**直接复制到源文件的代码**：
```cpp
int IxNodeHandle::remove(const char *key) {
    // 1. 找到 key 的位置
    int pos = lower_bound(key);
    // 2. 检查 key 是否存在
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_[0], file_hdr->col_tot_len_) == 0) {
        erase_pair(pos);  // 存在则删除
    }
    return get_size();
}
```

### IxNodeHandle 验证

这 8 个函数的测试包含在后续的 `b_plus_tree_insert_test` 和 `b_plus_tree_delete_test` 中，不需要单独验证。

---

## 3. IxIndexHandle 查找操作：2 个 TODO

**文件**：`src/index/ix_index_handle.cpp`

查找是 B+ 树最基本的操作，插入和删除都依赖于它。



查找是 B+ 树最基本的操作，插入和删除都依赖于它。理解查找是理解后续所有操作的基础。

### TODO 3.1：find_leaf_page

**位置**：`ix_index_handle.cpp` 第 167-175 行

**功能**：从根节点开始，沿着 B+ 树向下走，直到找到包含指定 key 的叶子节点。

**核心思路**：B+ 树的查找路径是唯一的 -- 从根到叶子。每到一个内部节点，用二分查找决定下一步走哪个子节点；到了叶子节点就停下来。

**代码**：

```cpp
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    // 1. 获取根节点（会 pin 住根页面）
    IxNodeHandle *node = fetch_node(file_hdr_->root_page_);

    // 2. 从根节点不断向下查找，直到找到叶子节点
    while (!node->is_leaf_page()) {
        // 内部节点：通过 internal_lookup 找到 key 应该走的子节点页号
        // internal_lookup 内部使用二分查找，返回第一个 >= key 的子节点
        page_id_t child_page_no = node->internal_lookup(key);

        // 获取子节点（会 pin 住子节点页面）
        IxNodeHandle *child = fetch_node(child_page_no);

        // 父节点已经用完了，unpin 它
        // is_dirty=false 因为查找不会修改任何节点
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);

        // 继续向下走
        node = child;
    }

    // 3. 找到叶子节点，返回
    // 第二个返回值（false）在本实验中暂不使用，与并发控制相关
    return std::make_pair(node, false);
}
```

**为什么这么写**：

1. **从根开始**：B+ 树的查找永远从根节点开始，这是唯一入口
2. **循环向下**：用 `while (!node->is_leaf_page())` 控制循环，因为只有叶子节点才存储实际的键值数据
3. **及时 unpin**：每拿到子节点后立即 unpin 父节点。BufferPoolManager 的 pin 数量有限，不及时 unpin 会导致页面无法被替换，最终可能耗尽缓冲池
4. **internal_lookup**：内部节点的查找返回的是子节点页号，不是 Rid。内部节点的 key 起"分隔"作用，告诉我们某个 key 属于哪个子树

---

### TODO 3.2：get_value

**位置**：`ix_index_handle.cpp` 第 185-193 行

**功能**：给定一个 key，查找它对应的 Rid（记录标识符，包含 page_no 和 slot_no）。

**核心思路**：先 `find_leaf_page` 定位到叶子节点，再在叶子节点内查找具体的 key。

**代码**：

```cpp
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    // 1. 找到 key 所在的叶子节点（会 pin 住叶子页面）
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, transaction);

    // 2. 在叶子节点中查找 key
    Rid *rid = nullptr;
    if (leaf->leaf_lookup(key, &rid)) {
        // 找到了，把 Rid 加入结果集
        // 注意这里用 *rid 解引用，因为 leaf_lookup 返回的是指针
        result->push_back(*rid);
    }

    // 3. 查找完成，unpin 叶子节点
    // is_dirty=false，因为查找不修改页面
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);

    // 返回是否找到了至少一个结果
    return !result->empty();
}
```

**为什么这么写**：

1. **结构化绑定** `auto [leaf, root_is_latched]`：C++17 语法，直接解构 `find_leaf_page` 返回的 `std::pair`
2. **leaf_lookup 的双重作用**：它返回 `bool` 表示是否找到，同时通过输出参数 `&rid` 返回找到的 Rid 指针
3. **结果用 vector 存储**：虽然主键索引中一个 key 对应一个 Rid，但接口设计为 vector，为将来支持非唯一索引（一个 key 对应多条记录）留有余地
4. **必须 unpin**：`find_leaf_page` 会 pin 住叶子节点，用完后必须 unpin，否则会内存泄漏（页面永远留在缓冲池中）

---

## 4. IxIndexHandle 插入操作：3 个 TODO

插入操作比查找复杂得多，因为当叶子节点满了需要分裂，分裂可能向上传播直到根节点。

### TODO 4.1：split

**位置**：`ix_index_handle.cpp` 第 202-211 行

**功能**：将一个已满的节点分裂为两个节点。右半部分移入新创建的节点。

**核心思路**：

- 计算分裂点（通常是 `size / 2`）
- 右半部分的键值对复制到新节点
- 更新旧节点的 size（只保留左半部分）
- 如果是叶子节点，维护叶子链表的 prev/next 指针
- 如果是内部节点，更新新节点所有子节点的 parent 指针

**代码**：

```cpp
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    // 1. 创建新节点（会 pin 住新页面）
    IxNodeHandle *new_node = create_node();

    // 2. 计算分裂点
    // split_key 是右半部分的起始索引
    // 例如：节点有 5 个 key [A, B, C, D, E]，split_key=2
    //       左半部分：[A, B]，右半部分：[C, D, E]
    int split_key = node->get_size() / 2;
    int right_size = node->get_size() - split_key;

    // 3. 把右半部分的键值对复制到新节点
    // get_key(split_key) 返回第 split_key 个 key 的地址
    // get_rid(split_key) 返回第 split_key 个 rid 的地址
    // insert_pairs 会连续复制 right_size 个键值对
    new_node->insert_pairs(0, node->get_key(split_key), node->get_rid(split_key), right_size);

    // 4. 初始化新节点的 page header
    // insert_pairs 已经将 num_key 增加了 right_size，无需再调用 set_size
    new_node->set_parent_page_no(node->get_parent_page_no());

    // 5. 旧节点只保留左半部分，缩小 size 即可
    // 无需清空右半部分的数据，因为 size 已经缩小，那些位置被视为无效
    node->set_size(split_key);

    // 6. 根据节点类型做特殊处理
    if (node->is_leaf_page()) {
        // --- 叶子节点：维护双向链表 ---
        new_node->page_hdr->is_leaf = true;

        // 新节点的 prev 指向旧节点
        new_node->set_prev_leaf(node->get_page_no());
        // 新节点的 next 指向旧节点原来的 next
        new_node->set_next_leaf(node->get_next_leaf());

        // 更新后继节点的 prev 指针（如果后继节点存在）
        IxNodeHandle *next = fetch_node(node->get_next_leaf());
        next->set_prev_leaf(new_node->get_page_no());
        buffer_pool_manager_->unpin_page(next->get_page_id(), true);

        // 旧节点的 next 指向新节点
        node->set_next_leaf(new_node->get_page_no());
    } else {
        // --- 内部节点：更新子节点的 parent 指针 ---
        new_node->page_hdr->is_leaf = false;
        // 新节点现在是右半部分子节点的新父亲
        for (int i = 0; i < new_node->get_size(); i++) {
            maintain_child(new_node, i);
        }
    }

    return new_node;
}
```

**为什么这么写**：

1. **分裂点取 `size / 2`**：保证左右两半尽量均匀。对于阶数为 n 的 B+ 树，每个节点最多存 n 个 key，分裂后左右各约 n/2 个
2. **insert_pairs 批量复制**：比逐个插入效率高，底层是 `memcpy`
3. **只修改 size 不清空数据**：这是一种优化。右半部分的脏数据还在物理页面上，但逻辑上已经无效（size 缩小后不会被访问到）
4. **叶子节点维护链表**：叶子节点之间通过 prev/next 形成双向链表，支持范围查询（`SELECT * WHERE key BETWEEN a AND b`）。分裂后必须更新链表关系
5. **内部节点维护 child parent**：`maintain_child` 会更新新节点的第 i 个子节点的 parent_page_no 为新节点。因为这些子节点原来属于旧节点，分裂后它们有了新的父亲
6. **unpin 后继节点时 `is_dirty=true`**：因为我们修改了后继节点的 prev 指针

---

### TODO 4.2：insert_into_parent

**位置**：`ix_index_handle.cpp` 第 226-234 行

**功能**：节点分裂后，需要把新节点的"分割 key"插入到父节点中。如果父节点也满了，递归分裂。

**核心思路**：

- 如果分裂的是根节点，需要创建新的根（树的高度 +1）
- 否则，在父节点中找到旧节点的位置，在其后插入新节点的 key
- 如果父节点也满了，递归调用 split + insert_into_parent

**代码**：

```cpp
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    // 1. 如果 old_node 是根节点，需要创建新的根
    if (old_node->is_root_page()) {
        IxNodeHandle *new_root = create_node();
        // 关键：create_node() memset 页面为0，parent 默认是 0 而非 INVALID_PAGE_ID(-1)
        // 不设置会导致 is_root_page() 返回 false，后续 fetch 到 page 0 死循环
        new_root->set_parent_page_no(IX_NO_PAGE);

        // 新根的第一个 key 是 old_node 的第一个 key，指向 old_node
        new_root->insert_pair(0, old_node->get_key(0), {.page_no = old_node->get_page_no()});
        // 新根的第二个 key 是 new_node 的第一个 key，指向 new_node
        new_root->insert_pair(1, new_node->get_key(0), {.page_no = new_node->get_page_no()});

        new_root->page_hdr->is_leaf = false;  // 根节点永远是内部节点

        // 更新全局的根节点页号
        update_root_page_no(new_root->get_page_no());

        // 设置子节点的 parent 指针
        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());

        // unpin 新根
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }

    // 2. 获取父节点（会 pin 住父页面）
    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());

    // 3. 找到 old_node 在父节点中的位置
    // find_child 遍历父节点的所有子节点，找到 page_no 匹配的那个
    int idx = parent->find_child(old_node);

    // 4. 在父节点中 old_node 位置之后插入新节点的 key
    // 例如：父节点 [..., old_key, ...]，插入后变为 [..., old_key, new_key, ...]
    parent->insert_pair(idx + 1, new_node->get_key(0), {.page_no = new_node->get_page_no()});

    // 5. 检查父节点是否也满了
    if (parent->get_size() == parent->get_max_size()) {
        // 父节点也满了，需要分裂
        IxNodeHandle *new_parent = split(parent);

        // 递归：把新分裂出来的父节点插入到祖父节点
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);

        // unpin 新分裂出来的父节点
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }

    // 6. unpin 当前父节点
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}
```

**为什么这么写**：

1. **根节点分裂的特殊情况**：当根节点分裂时，B+ 树的高度增加 1。新创建的根只有 2 个子节点（旧根的左半部分和右半部分），这是合法的（根节点最少可以只有 2 个子节点）
2. **插入位置是 `idx + 1`**：因为新节点是旧节点分裂出来的右半部分，它应该紧跟在旧节点后面
3. **递归分裂**：这是 B+ 树插入的核心。分裂可能一路传播到根节点，每次传播都会创建一个新节点。最坏情况下，插入操作会导致 log(n) 次分裂
4. **必须设置 `set_parent_page_no(IX_NO_PAGE)`**：`create_node()` 的页面被 `memset` 为零，parent 默认是 0 而非 `INVALID_PAGE_ID`(-1)。如果不设置，`is_root_page()` 会返回 false，后续会 fetch 到 page 0（文件头），导致 `find_child` 断言失败。这是最常见的 B+ 树调试陷阱
5. **unpin 的时机**：`split` 和 `insert_into_parent` 内部会 pin 住新节点，用完后必须 unpin。注意 `new_parent` 是在 `split` 中被 pin 住的，用完后要 unpin

---

### TODO 4.3：insert_entry

**位置**：`ix_index_handle.cpp` 第 242-250 行

**功能**：B+ 树插入操作的入口函数。将一个键值对插入到索引中。

**核心思路**：

- 找到 key 应该插入的叶子节点
- 在叶子节点中插入键值对
- 如果叶子节点满了，分裂并向上维护

**代码**：

```cpp
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    // 1. 找到 key 应该插入的叶子节点（会 pin 住叶子页面）
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::INSERT, transaction);

    // 2. 在叶子节点中插入键值对
    // insert 内部会找到正确的插入位置（保持有序），并移动后面的元素
    leaf->insert(key, value);

    // 关键：如果插入在叶子节点的第一个位置，需要向上更新父节点链的key
    // 否则父节点的分隔key就是过时的，check_tree 会报 key 不匹配
    maintain_parent(leaf);

    // 3. 检查叶子节点是否满了
    if (leaf->get_size() == leaf->get_max_size()) {
        // 叶子节点满了，需要分裂
        IxNodeHandle *new_leaf = split(leaf);

        // 如果分裂的是最右边的叶子节点，更新 file_hdr 的 last_leaf
        // last_leaf 用于支持从右向左的范围扫描
        if (file_hdr_->last_leaf_ == leaf->get_page_no()) {
            file_hdr_->last_leaf_ = new_leaf->get_page_no();
        }

        // 把新节点的分割 key 插入到父节点
        // 这个过程可能递归触发更多分裂
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);

        // unpin 新叶子节点（split 会 pin 住它）
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    }

    // 4. unpin 叶子节点（find_leaf_page 会 pin 住它）
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);

    // 返回插入位置所在的页号
    return leaf->get_page_no();
}
```

**为什么这么写**：

1. **先插入再检查是否满**：这是一种常见的实现方式。先插入，如果满了再分裂。另一种方式是"预分裂"（先检查，满了先分裂再插入），但前者实现更简单
2. **更新 last_leaf**：叶子节点链表的尾指针需要维护。当最右边的叶子分裂时，新节点变成了最右边的叶子
3. **insert 保持有序**：`leaf->insert(key, value)` 内部会用二分查找找到插入位置，然后用 `memmove` 移动后面的元素，保证叶子节点中的 key 始终有序
4. **分裂的级联效应**：`split` + `insert_into_parent` 的组合可以处理任意深度的分裂。最坏情况下，一次插入会导致从叶子到根的每层都分裂
5. **返回值是 leaf->get_page_no()**：返回插入所在的页号，方便上层逻辑（如事务管理）知道数据被写到了哪里

---

## 5. IxIndexHandle 删除操作：5 个 TODO

**文件**：`src/index/ix_index_handle.cpp`

关键概念：
- **coalesce（合并）**：把右边的节点合并到左边，然后删除右边节点
- **redistribute（借位）**：从兄弟节点借一个键值对
- 节点的 `min_size = max_size / 2`（根节点特殊：内部节点 min_size = 2，叶子节点 min_size = 1）



### TODO 5.1：adjust_root

- 位置：`ix_index_handle.cpp` 第 298-305 行
- 根节点删除键值对后的处理

```cpp
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    // 情况1：根节点是内部节点，且只有一个子节点
    // 那么把这个子节点提升为新的根节点
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        page_id_t child_page_no = old_root_node->remove_and_return_only_child();
        IxNodeHandle *new_root = fetch_node(child_page_no);
        new_root->set_parent_page_no(INVALID_PAGE_ID);
        update_root_page_no(child_page_no);
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        release_node_handle(*old_root_node);
        buffer_pool_manager_->unpin_page(old_root_node->get_page_id(), true);
        return true;  // 旧根节点需要被删除
    }
    // 情况2：根节点是叶子节点，且键值对为空
    // 说明整棵树被清空了
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        release_node_handle(*old_root_node);
        buffer_pool_manager_->unpin_page(old_root_node->get_page_id(), true);
        update_root_page_no(IX_NO_PAGE);
        return true;
    }
    // 其他情况：根节点不需要调整
    return false;
}
```

### TODO 5.2：redistribute

- 位置：`ix_index_handle.cpp` 第 321-327 行
- 从兄弟节点借一个键值对到 node

```cpp
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    // index == 0：neighbor 是 node 的后继（neighbor 在右边）
    if (index == 0) {
        // 从右边兄弟借第一个键值对到 node 的末尾
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);
        // 更新父节点中 node 和 neighbor 之间的 key 为 neighbor 的新第一个 key
        parent->set_key(1, neighbor_node->get_key(0));  // index=0 时 parent 的第 1 个 key 对应 neighbor
        // 如果 node 是内部节点，更新刚移动过来的子节点的 parent
        if (!node->is_leaf_page()) {
            maintain_child(node, node->get_size() - 1);
        }
    } else {
        // neighbor 在左边：从左边兄弟借最后一个键值对到 node 的开头
        node->insert_pair(0, neighbor_node->get_key(neighbor_node->get_size() - 1),
                          *neighbor_node->get_rid(neighbor_node->get_size() - 1));
        neighbor_node->erase_pair(neighbor_node->get_size() - 1);
        // 更新父节点中 neighbor 和 node 之间的 key 为 node 的新第一个 key
        parent->set_key(index, node->get_key(0));
        // 如果 node 是内部节点，更新刚移动过来的子节点的 parent
        if (!node->is_leaf_page()) {
            maintain_child(node, 0);
        }
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), true);
    buffer_pool_manager_->unpin_page(neighbor_node->get_page_id(), true);
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}
```

### TODO 5.3：coalesce

- 位置：`ix_index_handle.cpp` 第 343-352 行
- 将 node 合并到 neighbor_node（node 在右边）
- 返回 parent 是否也需要被删除

```cpp
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // 1. 确保 neighbor_node 在左边，node 在右边
    //    如果 index == 0，说明 node 在左边，需要交换
    if (index == 0) {
        std::swap(neighbor_node, node);
        index = 1;
    }
    IxNodeHandle *left = *neighbor_node;
    IxNodeHandle *right = *node;
    // 2. 把 right 的所有键值对移到 left 的末尾
    left->insert_pairs(left->get_size(), right->get_key(0), right->get_rid(0), right->get_size());
    // 3. 更新 left 最后一个子节点的 parent（如果是内部节点）
    if (!left->is_leaf_page()) {
        for (int i = left->get_size() - right->get_size(); i < left->get_size(); i++) {
            maintain_child(left, i);
        }
    }
    // 4. 如果是叶子节点，更新叶子链表
    if (right->is_leaf_page()) {
        left->set_next_leaf(right->get_next_leaf());
        IxNodeHandle *next = fetch_node(right->get_next_leaf());
        next->set_prev_leaf(left->get_page_no());
        buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    }
    // 5. 从父节点删除 right 对应的键值对
    (*parent)->erase_pair(index);
    // 6. 如果 right 是最右叶子，更新 last_leaf
    if (file_hdr_->last_leaf_ == right->get_page_no()) {
        file_hdr_->last_leaf_ = left->get_page_no();
    }
    // 7. 释放 right 节点
    release_node_handle(*right);
    buffer_pool_manager_->unpin_page(right->get_page_id(), true);
    // 8. unpin left 和 parent
    buffer_pool_manager_->unpin_page(left->get_page_id(), true);
    buffer_pool_manager_->unpin_page((*parent)->get_page_id(), true);
    // 9. 递归检查父节点是否需要合并或重分配
    return coalesce_or_redistribute(*parent, transaction, root_is_latched);
}
```

### TODO 5.4：coalesce_or_redistribute

- 位置：`ix_index_handle.cpp` 第 278-290 行
- 判断是合并还是借位

```cpp
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    // 1. 如果 node 是根节点，特殊处理
    if (node->is_root_page()) {
        bool ret = adjust_root(node);
        // 关键：adjust_root 返回 false 时不会 unpin 节点，需要手动 unpin
        // 否则根节点永远被 pin 住，最终耗尽缓冲池导致死锁
        if (!ret) {
            buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        }
        return ret;
    }
    // 2. 如果 node 的 size >= min_size，不需要任何操作
    if (node->get_size() >= node->get_min_size()) {
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        return false;
    }
    // 3. 获取父节点
    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    // 4. 找到 node 在父节点中的位置
    int index = parent->find_child(node);
    // 5. 寻找兄弟节点（优先选前驱，即左边的兄弟）
    IxNodeHandle *neighbor;
    if (index == 0) {
        // node 是第一个子节点，没有前驱，只能选后继
        neighbor = fetch_node(parent->value_at(1));
    } else {
        // 选前驱
        neighbor = fetch_node(parent->value_at(index - 1));
    }
    // 6. 判断：两个节点的键值对之和能否支撑两个节点
    //    如果 node.size + neighbor.size >= 2 * min_size，借位即可
    if (node->get_size() + neighbor->get_size() >= 2 * node->get_min_size()) {
        redistribute(neighbor, node, parent, index);
        return false;
    }
    // 7. 否则需要合并
    return coalesce(&neighbor, &node, &parent, index, transaction, root_is_latched);
}
```

### TODO 5.5：delete_entry

- 位置：`ix_index_handle.cpp` 第 257-265 行
- B+ 树删除的入口函数

```cpp
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    // 1. 找到 key 所在的叶子节点
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    // 2. 在叶子节点中删除键值对
    leaf->remove(key);
    // 关键：如果删除了叶子节点的第一个key，需要向上更新父节点链的key
    // 否则父节点的分隔key就是过时的，会导致 check_tree 报 key 不匹配
    maintain_parent(leaf);
    // 3. 如果删除后节点太小，需要合并或借位
    coalesce_or_redistribute(leaf, transaction, &root_is_latched);
    return true;
}
```

---

## 6. IxIndexHandle 范围查找：2 个 TODO

### TODO 6.1：IxIndexHandle::lower_bound

- 位置：`ix_index_handle.cpp` 第 380-383 行

```cpp
Iid IxIndexHandle::lower_bound(const char *key) {
    // 1. 找到 key 所在的叶子节点
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    // 2. 在叶子节点中找第一个 >= key 的位置
    int idx = leaf->lower_bound(key);
    // 3. 构造 Iid 返回
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = idx};
    // 4. unpin
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}
```

### TODO 6.2：IxIndexHandle::upper_bound

- 位置：`ix_index_handle.cpp` 第 391-394 行

```cpp
Iid IxIndexHandle::upper_bound(const char *key) {
    // 1. 找到 key 所在的叶子节点
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    // 2. 在叶子节点中找第一个 > key 的位置
    int idx = leaf->upper_bound(key);
    // 3. 构造 Iid
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = idx};
    // 4. 如果 idx 超出当前叶子节点范围，跳到下一个叶子
    if (idx >= leaf->get_size() && leaf->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
        iid.page_no = leaf->get_next_leaf();
        iid.slot_no = 0;
    }
    // 5. unpin
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}
```

---

## 7. SmManager DDL：2 个 TODO

### TODO 7.1：create_index

- 位置：`sm_manager.cpp` 第 200-202 行

```cpp
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    // 1. 获取表的元数据
    TabMeta &tab = db_.get_table(tab_name);
    // 2. 找到索引列的元数据
    std::vector<ColMeta> index_cols;
    for (auto &col_name : col_names) {
        index_cols.push_back(*tab.get_col(col_name));
    }
    // 3. 检查索引是否已存在
    if (ix_manager_->exists(tab_name, index_cols)) {
        throw IndexExistsError(tab_name, col_names);
    }
    // 4. 创建索引文件
    ix_manager_->create_index(tab_name, index_cols);
    // 5. 更新表的元数据
    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_num = index_cols.size();
    int col_tot_len = 0;
    for (auto &col : index_cols) col_tot_len += col.len;
    index_meta.col_tot_len = col_tot_len;
    index_meta.cols = index_cols;
    tab.indexes.push_back(index_meta);
}
```

### TODO 7.2：drop_index（两个重载）

- 位置：`sm_manager.cpp` 第 210-222 行

代码（第一个重载，参数是 `col_names`）：

```cpp
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    std::vector<ColMeta> index_cols;
    for (auto &col_name : col_names) {
        index_cols.push_back(*tab.get_col(col_name));
    }
    // 删除索引文件
    ix_manager_->destroy_index(tab_name, index_cols);
    // 从表元数据中移除
    auto index_it = tab.get_index_meta(col_names);
    tab.indexes.erase(index_it);
}
```

代码（第二个重载，参数是 `ColMeta`）：

```cpp
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    ix_manager_->destroy_index(tab_name, cols);
    TabMeta &tab = db_.get_table(tab_name);
    // 从列名构建 col_names
    std::vector<std::string> col_names;
    for (auto &col : cols) {
        col_names.push_back(col.name);
    }
    auto index_it = tab.get_index_meta(col_names);
    tab.indexes.erase(index_it);
}
```

---

## 8. B+ 树并发控制：3 个 TODO

并发控制是 Lab2 的**任务4**，占 30 分。本实现采用**粗粒度（Tree 级）**并发，用一把互斥锁保护整棵 B+ 树的所有读写操作。

### TODO 8.1：get_value 加锁

**位置**：`ix_index_handle.cpp` 第 187 行

```cpp
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};  // 加读锁，与写操作互斥
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, transaction);
    // ... 其余代码不变 ...
}
```

### TODO 8.2：insert_entry 加锁

**位置**：`ix_index_handle.cpp` 第 273 行

```cpp
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};  // 加写锁，与读/写操作互斥
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    // ... 其余代码不变 ...
}
```

### TODO 8.3：delete_entry 加锁

**位置**：`ix_index_handle.cpp` 第 293 行

```cpp
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};  // 加写锁，与读/写操作互斥
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    // ... 其余代码不变 ...
}
```

**为什么这么写**：
- `IxIndexHandle` 已有一个 `std::mutex root_latch_` 成员，直接复用
- `std::scoped_lock` 在作用域结束时自动解锁，异常安全
- 粗粒度锁实现简单，足够通过并发测试（插入/删除/查找互斥即可）
- 注意：这三处加锁后，`coalesce_or_redistribute` 等内部函数**不需要**再加锁（它们总是在锁内被调用）

---

## 9. 完整测试流程（验证通过后执行）

```bash
cd build

**第 1 步：插入测试（30 分）**
make b_plus_tree_insert_test && ./bin/b_plus_tree_insert_test

**第 2 步：删除测试（40 分）**
make b_plus_tree_delete_test && ./bin/b_plus_tree_delete_test

**第 3 步：并发测试（30 分，需要先完成并发控制部分）**
make b_plus_tree_concurrent_test && ./bin/b_plus_tree_concurrent_test
```

注意：测试前需要先实现 `sm_manager.cpp` 中的 `create_index` 函数。

---

## 10. 调试技巧

### 10.1 常见编译错误

- `no matching function for insert_pairs`：检查参数类型，key 是 `const char*`，rid 是 `const Rid*`
- `ix_compare` 未声明：检查是否包含了 `ix_index_handle.h`
- `get_size()` 在 const 函数中调用报错：改用 `page_hdr->num_key` 直接访问

### 10.2 常见运行时错误

- **Segmentation fault in split**：检查 split 后是否正确更新了叶子链表的 prev/next
- **Assertion failed in find_child**：检查 `create_node` 后新节点的 parent 是否为 `IX_NO_PAGE`（-1）而非 0；若不设则 `is_root_page()` 返回 false，导致 fetch 到 page 0
- **死循环/卡死**：检查所有路径是否都 unpin 了页面。尤其是 `coalesce_or_redistribute` 中 `adjust_root` 返回 false 时需要手动 unpin
- **Segment fault in internal_lookup**：检查是否处理了 `upper_bound` 返回 0 的情况（值被越界访问）

### 10.3 常见逻辑错误

- **分裂后丢失数据**：split 时检查 split_key 的计算，确保左右两部分都不为空
- **合并后数据错乱**：coalesce 时确保先交换让 neighbor 在左边
- **叶子链表断裂**：split/coalesce 后检查 `prev_leaf` 和 `next_leaf` 是否正确
- **父节点 key 不一致**：`insert_entry` 和 `delete_entry` 中如果修改了叶子节点的第一个 key，必须调用 `maintain_parent(leaf)` 向上传播更新

### 10.4 调试命令

```bash
gdb ./bin/b_plus_tree_insert_test
(gdb) break ix_index_handle.cpp:202  # 在 split 打断点
(gdb) run
(gdb) print node->get_size()         # 打印节点大小
(gdb) print *node->get_key(0)        # 打印第一个 key
```

---

## 附录：各函数速查表

| 模块 | 函数 | 文件位置 | 核心操作 |
|------|------|----------|----------|
| IxNodeHandle | lower_bound | ix_index_handle.cpp:21 | 二分查找第一个 >= target |
| IxNodeHandle | upper_bound | ix_index_handle.cpp:35 | 二分查找第一个 > target |
| IxNodeHandle | leaf_lookup | ix_index_handle.cpp:51 | 叶子按键查找 |
| IxNodeHandle | internal_lookup | ix_index_handle.cpp:66 | 内部节点查子树 |
| IxNodeHandle | insert_pairs | ix_index_handle.cpp:89 | 批量插入键值对 |
| IxNodeHandle | insert | ix_index_handle.cpp:105 | 单个插入 |
| IxNodeHandle | erase_pair | ix_index_handle.cpp:120 | 删除键值对 |
| IxNodeHandle | remove | ix_index_handle.cpp:134 | 按键删除 |
| IxIndexHandle | find_leaf_page | ix_index_handle.cpp:167 | 从根到叶查找 |
| IxIndexHandle | get_value | ix_index_handle.cpp:185 | 获取 key 对应 Rid |
| IxIndexHandle | split | ix_index_handle.cpp:202 | 分裂节点 |
| IxIndexHandle | insert_into_parent | ix_index_handle.cpp:226 | 分裂后向上插入 |
| IxIndexHandle | insert_entry | ix_index_handle.cpp:242 | 插入入口 |
| IxIndexHandle | delete_entry | ix_index_handle.cpp:257 | 删除入口 |
| IxIndexHandle | coalesce_or_redistribute | ix_index_handle.cpp:278 | 合并/借位判断 |
| IxIndexHandle | adjust_root | ix_index_handle.cpp:298 | 根节点调整 |
| IxIndexHandle | redistribute | ix_index_handle.cpp:321 | 借位 |
| IxIndexHandle | coalesce | ix_index_handle.cpp:343 | 合并 |
| IxIndexHandle | lower_bound | ix_index_handle.cpp:380 | FindLeafPage + lower_bound |
| IxIndexHandle | upper_bound | ix_index_handle.cpp:391 | FindLeafPage + upper_bound |
| SmManager | create_index | sm_manager.cpp:200 | 创建索引 |
| SmManager | drop_index | sm_manager.cpp:210 | 删除索引 |
| 并发控制 | get_value + scoped_lock | ix_index_handle.cpp:187 | 加锁读 |
| 并发控制 | insert_entry + scoped_lock | ix_index_handle.cpp:273 | 加锁写 |
| 并发控制 | delete_entry + scoped_lock | ix_index_handle.cpp:293 | 加锁写 |
