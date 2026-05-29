# Lab2 索引管理实验报告

---

## 一、模块介绍

Lab2 实现 RucBase 教学数据库的**索引管理模块**，为数据表提供基于 B+ 树的索引能力。

### 模块组成

| 类 | 作用 | 文件位置 |
|----|------|----------|
| `IxManager` | 索引文件生命周期管理（创建/打开/关闭/删除） | `src/index/ix_manager.h` |
| `IxIndexHandle` | B+ 树操作入口（查找/插入/删除），并发控制 | `src/index/ix_index_handle.cpp` |
| `IxNodeHandle` | 单节点内部操作（二分查找/插入/删除键值对） | `src/index/ix_index_handle.h` |
| `IxScan` | 叶子节点范围扫描 | `src/index/ix_scan.h` |
| `SmManager` (部分) | DDL 操作：`create_index` / `drop_index` | `src/system/sm_manager.cpp` |

### 模块层次

```
SQL: CREATE INDEX / DROP INDEX
        ↓
  SmManager (DDL 入口)
        ↓
  IxManager (文件管理)
        ↓
  IxIndexHandle (B+ 树逻辑)
        ↓
  IxNodeHandle (节点操作)
        ↓
  BufferPoolManager + DiskManager (页面缓存 + 磁盘 I/O)
```

---

## 二、知识背景与相关技术

### 2.1 B+ 树数据结构

B+ 树是一种多路平衡搜索树，特点：

- **叶子节点**存储实际数据（key → Rid），内部节点仅存储索引
- 叶子节点通过**双向链表**连接（prev_leaf / next_leaf），支持范围扫描
- 所有叶子节点在同一层，查找路径长度固定
- 每个节点最多存 `btree_order` 个键值对，最少存 `btree_order / 2` 个（50% 填充率）

### 2.2 索引文件页面布局

```
Page 0: IxFileHdr          — 文件头，存 root_page / num_pages / btree_order 等
Page 1: LEAF_HEADER_PAGE   — 叶子链表哨兵节点
Page 2: IX_INIT_ROOT_PAGE  — 初始根节点（空叶子）
Page 3+: 数据节点
```

### 2.3 节点内存布局

每个 Page（4096 字节）内部：

```
[IxPageHdr (parent, num_key, is_leaf, prev_leaf, next_leaf)]
[keys 数组 (每个 key 占 col_tot_len 字节)]
[rids 数组 (每个 Rid 占 8 字节)]
```

`btree_order` 计算公式：
```
|page_hdr| + (|attr| + |rid|) * (n + 1) <= PAGE_SIZE
btree_order = (4096 - sizeof(IxPageHdr)) / (col_tot_len + sizeof(Rid)) - 1
```

### 2.4 关键辅助函数

| 函数 | 作用 |
|------|------|
| `fetch_node(page_no)` | 从缓冲池获取页面，构造 IxNodeHandle（pin 住） |
| `create_node()` | 分配新页面，构造 IxNodeHandle（pin 住） |
| `maintain_parent(node)` | 从 node 向上递归更新父节点的第一个 key |
| `maintain_child(node, i)` | 设置 node 第 i 个子节点的 parent 指针 |
| `release_node_handle(node)` | 删除节点后更新 num_pages |
| `ix_compare(a, b, type, len)` | 比较两个 key 的大小 |

### 2.5 并发控制

粗粒度（Tree 级）方案：使用 `std::mutex root_latch_` 对整个 B+ 树加锁，所有读写操作互斥。简单但性能受限于单棵树。细粒度（Page 级）蟹行协议为选做内容。

---

## 三、实验内容与任务要求

### 任务1：B+ 树的查找（节点内 + 树级）

- `lower_bound` / `upper_bound`：二分查找第一个 >= 或 > target 的 key
- `leaf_lookup`：叶子节点按键查找 Rid
- `internal_lookup`：内部节点查找目标子树
- `find_leaf_page`：从根到叶遍历
- `get_value`：公共查找接口

### 任务2：B+ 树的插入

- `insert_pairs` / `insert`：节点内插入键值对（memmove + memcpy）
- `split`：分裂满节点，维护叶子链表或子节点 parent
- `insert_into_parent`：递归向上插入，必要时创建新根
- `insert_entry`：插入入口函数

### 任务3：B+ 树的删除

- `erase_pair` / `remove`：节点内删除键值对
- `adjust_root`：根节点收缩或清空
- `redistribute`：从兄弟节点借键值对
- `coalesce`：合并两个节点并递归向上
- `coalesce_or_redistribute`：判断借位还是合并
- `delete_entry`：删除入口函数

### 任务4：并发控制（粗粒度）

- 在 `get_value` / `insert_entry` / `delete_entry` 中加 `std::scoped_lock`

### 计分标准

| 任务 | 测试文件 | 分值 |
|------|---------|------|
| 查找 + 插入 | `b_plus_tree_insert_test` | 30 |
| 删除 | `b_plus_tree_delete_test` | 40 |
| 并发控制 | `b_plus_tree_concurrent_test` | 30 |
| **总计** | | **100** |

---

## 四、实验步骤与具体实现

### 4.1 节点内操作（8 个 TODO）

#### lower_bound / upper_bound（二分查找）

```cpp
int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0, right = page_hdr->num_key;  // 注意：const 函数不能调用 get_size()
    while (left < right) {
        int mid = left + (right - left) / 2;
        if (ix_compare(get_key(mid), target, ...) < 0)
            left = mid + 1;
        else
            right = mid;
    }
    return left;  // 返回第一个 >= target 的位置
}
```

`upper_bound` 的区别仅在于条件：`<= 0` → `left = mid + 1`（跳过等于的 key）。

#### insert_pairs（批量插入）

```cpp
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    // 腾出 pos 位置的空间：把 [pos, num_key) 移到 [pos+n, num_key+n)
    memmove(get_key(pos + n), get_key(pos), (num_key - pos) * col_tot_len_);
    memcpy(get_key(pos), key, n * col_tot_len_);         // 复制新 key
    memmove(get_rid(pos + n), get_rid(pos), ...);         // rids 同理
    memcpy(get_rid(pos), rid, n * sizeof(Rid));
    page_hdr->num_key += n;
}
```

关键：`memmove` 处理重叠区域，`memcpy` 不处理。keys 和 rids 是两个独立数组，分别移动。

#### erase_pair（删除）

```cpp
void IxNodeHandle::erase_pair(int pos) {
    memmove(get_key(pos), get_key(pos + 1), (num_key - pos - 1) * col_tot_len_);
    memmove(get_rid(pos), get_rid(pos + 1), ...);
    page_hdr->num_key--;
}
```

### 4.2 查找操作（2 个 TODO）

#### find_leaf_page

```cpp
std::pair<IxNodeHandle*, bool> IxIndexHandle::find_leaf_page(...) {
    IxNodeHandle *node = fetch_node(file_hdr_->root_page_);
    while (!node->is_leaf_page()) {
        page_id_t child = node->internal_lookup(key);
        IxNodeHandle *child_node = fetch_node(child);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        node = child_node;
    }
    return {node, false};
}
```

每层获取子节点后立即 unpin 父节点，防止缓冲池耗尽。

#### get_value

```cpp
bool IxIndexHandle::get_value(...) {
    std::scoped_lock lock{root_latch_};         // 并发控制
    auto [leaf, _] = find_leaf_page(key, FIND, ...);
    Rid *rid;
    bool found = leaf->leaf_lookup(key, &rid);
    if (found) result->push_back(*rid);
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return found;
}
```

### 4.3 插入操作（3 个 TODO）

#### split（分裂）

```cpp
IxNodeHandle* IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    int mid = node->get_size() / 2;
    int n = node->get_size() - mid;
    new_node->insert_pairs(0, node->get_key(mid), node->get_rid(mid), n);
    new_node->set_parent_page_no(node->get_parent_page_no());
    node->set_size(mid);  // 旧节点缩小

    if (node->is_leaf_page()) {
        // 叶子：维护 prev/next 双向链表
        new_node->page_hdr->is_leaf = true;
        new_node->set_prev_leaf(node->get_page_no());
        new_node->set_next_leaf(node->get_next_leaf());
        // 更新后继节点的 prev
        IxNodeHandle *next = fetch_node(node->get_next_leaf());
        next->set_prev_leaf(new_node->get_page_no());
        buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        node->set_next_leaf(new_node->get_page_no());
    } else {
        // 内部节点：更新新节点所有子节点的 parent
        for (int i = 0; i < new_node->get_size(); i++)
            maintain_child(new_node, i);
    }
    return new_node;
}
```

#### insert_into_parent（递归向上插入）

```cpp
void IxIndexHandle::insert_into_parent(old_node, key, new_node, ...) {
    if (old_node->is_root_page()) {
        IxNodeHandle *new_root = create_node();
        new_root->set_parent_page_no(IX_NO_PAGE);  // ★ 关键：否则 fetch 到 page 0
        new_root->insert_pair(0, old_node->get_key(0), {old_node->get_page_no()});
        new_root->insert_pair(1, new_node->get_key(0), {new_node->get_page_no()});
        new_root->page_hdr->is_leaf = false;
        update_root_page_no(new_root->get_page_no());
        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }
    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    int idx = parent->find_child(old_node);
    parent->insert_pair(idx + 1, new_node->get_key(0), {new_node->get_page_no()});
    if (parent->get_size() == parent->get_max_size()) {
        IxNodeHandle *new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, ...);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}
```

### 4.4 删除操作（5 个 TODO）

#### coalesce_or_redistribute（借位/合并判断）

```cpp
bool IxIndexHandle::coalesce_or_redistribute(node, ...) {
    if (node->is_root_page()) {
        bool ret = adjust_root(node);
        if (!ret) buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // ★ 关键
        return ret;
    }
    if (node->get_size() >= node->get_min_size()) {
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        return false;
    }
    // 找兄弟节点（优先前驱）
    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int idx = parent->find_child(node);
    IxNodeHandle *neighbor = (idx == 0) ? fetch_node(parent->value_at(1))
                                        : fetch_node(parent->value_at(idx - 1));
    if (node->size + neighbor->size >= 2 * min_size) {
        redistribute(neighbor, node, parent, idx);
        return false;
    }
    return coalesce(&neighbor, &node, &parent, idx, ...);
}
```

#### adjust_root（根收缩）

```cpp
bool IxIndexHandle::adjust_root(old_root) {
    // 情况1：内部节点只剩1个孩子 → 提升孩子为新根
    if (!old_root->is_leaf_page() && old_root->get_size() == 1) {
        page_id_t child = old_root->remove_and_return_only_child();
        IxNodeHandle *new_root = fetch_node(child);
        new_root->set_parent_page_no(INVALID_PAGE_ID);
        update_root_page_no(child);
        release_node_handle(*old_root);
        buffer_pool_manager_->unpin_page(old_root->get_page_id(), true);
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return true;
    }
    // 情况2：叶子为空 → 树清空
    if (old_root->is_leaf_page() && old_root->get_size() == 0) {
        release_node_handle(*old_root);
        buffer_pool_manager_->unpin_page(old_root->get_page_id(), true);
        update_root_page_no(IX_NO_PAGE);
        return true;
    }
    return false;  // 无需调整
}
```

#### redistribute（借位）

- `idx == 0`（node 在左，借后继）：取后继第一个键值对放入 node 末尾，更新 parent key[1]
- `idx > 0`（node 在右，借前驱）：取前驱最后一个键值对放入 node 开头，更新 parent key[idx]

#### coalesce（合并）

- 确保 neighbor 在左、node 在右
- 将 node 所有键值对移到 neighbor 末尾
- 内部节点需更新子节点 parent（`maintain_child`）
- 叶子节点需更新 prev/next 链表
- 递归调用 `coalesce_or_redistribute(parent)`

### 4.5 范围查找（2 个 TODO）

```cpp
Iid IxIndexHandle::lower_bound(const char *key) {
    auto [leaf, _] = find_leaf_page(key, FIND, nullptr);
    int idx = leaf->lower_bound(key);
    Iid iid = {leaf->get_page_no(), idx};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}
```

### 4.6 SmManager DDL（2 个 TODO）

```cpp
void SmManager::create_index(tab_name, col_names, ...) {
    TabMeta &tab = db_.get_table(tab_name);
    // 收集索引列元数据
    std::vector<ColMeta> index_cols;
    for (auto &name : col_names)
        index_cols.push_back(*tab.get_col(name));
    // 检查重复
    if (ix_manager_->exists(tab_name, index_cols))
        throw IndexExistsError(tab_name, col_names);
    // 创建索引文件
    ix_manager_->create_index(tab_name, index_cols);
    // 更新表元数据
    IndexMeta meta = {tab_name, col_tot_len, col_num, index_cols};
    tab.indexes.push_back(meta);
}
```

### 4.7 并发控制（3 个 TODO）

在三个入口函数开头添加 `std::scoped_lock lock{root_latch_};`：
- `get_value` — 加锁读
- `insert_entry` — 加锁写
- `delete_entry` — 加锁写

`root_latch_` 是 `IxIndexHandle` 已有的 `std::mutex` 成员。

---

## 五、测试与性能分析

### 5.1 测试结果

```bash
cd build

# 插入测试（30分）
make b_plus_tree_insert_test && ./bin/b_plus_tree_insert_test
# 结果：2/2 PASSED (44ms)
# - InsertTest: 10 键，order=3，验证 get_value
# - LargeScaleTest: 10000 键随机插入，order=256，验证 IxScan 顺序

# 删除测试（40分）
make b_plus_tree_delete_test && ./bin/b_plus_tree_delete_test
# 结果：3/3 PASSED (194ms)
# - InsertAndDeleteTest1/2: 插入->删除->验证
# - LargeScaleTest: ~12000 插入 + ~7800 删除交替，验证 check_tree 结构

# 并发测试（30分）
make b_plus_tree_concurrent_test && ./bin/b_plus_tree_concurrent_test
# 结果：2/2 PASSED (11468ms)
# - InsertScaleTest: 多线程并发插入 1~10000
# - MixScaleTest: 多线程并发插入+删除
```

**全部 7 个测试通过，总分 100/100。**

### 5.2 性能分析

| 测试 | 数据量 | 耗时 | 说明 |
|------|--------|------|------|
| InsertTest | 10 keys, order=3 | 5ms | 小规模，多次分裂 |
| LargeScaleTest (insert) | 10000 keys, order=256 | 38ms | 大阶数，分裂少 |
| LargeScaleTest (delete) | ~20000 ops | 184ms | 混合操作，频繁借位/合并 |
| ConcurrentInsert | 10000 keys | 4778ms | 16 线程竞争锁 |
| ConcurrentMix | 10000+9900 ops | 6690ms | 16 线程，锁竞争加剧 |

粗粒度锁导致并发测试中大量时间花在互斥等待上，这是预期行为。

---

## 六、遇到问题与解决方案

### 问题1：`find_child` 断言失败，发现 parent 是 page 0

**现象**：插入第 5 个 key 后 `assert(rid_idx < page_hdr->num_key)` 失败，调试发现 parent 节点变成 page 0（文件头）。

**根因**：`create_node()` 调用 `memset` 清零页面，`page_hdr->parent` 默认为 0 而非 `INVALID_PAGE_ID`(-1)。`is_root_page()` 判断 `parent == -1`，但新根节点的 parent 是 0，导致 `insert_into_parent` 认为它不是根，转而 `fetch_node(0)`（文件头）。

**解决**：在 `insert_into_parent` 创建新根后添加 `new_root->set_parent_page_no(IX_NO_PAGE)`。

### 问题2：删除测试中电脑卡死

**现象**：运行 `b_plus_tree_delete_test` 后电脑无响应。

**根因**：`coalesce_or_redistribute` 中 `adjust_root` 返回 false 时（根不需要删除），没有 unpin 节点。每次删除操作泄漏一个 pin，积累后缓冲池耗尽 → 死锁。

**解决**：在 `coalesce_or_redistribute` 中添加 `if (!ret) buffer_pool_manager_->unpin_page(...)`。

### 问题3：LargeScaleTest 报 key 不匹配（1900 vs 1903）

**现象**：`check_tree` 断言 `node_key == child_first_key` 失败。

**根因**：`insert_entry` 和 `delete_entry` 在修改叶子节点的第一个 key 后（插入到 pos 0 或删除 pos 0 的 key），没有调用 `maintain_parent(leaf)` 向上传播更新。

**解决**：在两个函数中分别添加 `maintain_parent(leaf)` 调用。

### 问题4：`internal_lookup` 段错误

**现象**：偶发 segfault，出现在 `value_at(idx - 1)`。

**根因**：`upper_bound` 当 target < 所有 key 时返回 0，`value_at(-1)` 越界。

**解决**：添加 `if (idx == 0) return value_at(0)` 保护。

### 问题5：并发测试数据丢失

**现象**：`get_value` 无法找到刚插入的 key。

**根因**：多线程同时修改 B+ 树结构，无任何同步机制。

**解决**：在 `get_value` / `insert_entry` / `delete_entry` 中使用已有的 `root_latch_` 互斥锁，实现粗粒度并发控制。

---

## 七、实验总结

### 核心收获

1. **B+ 树实现细节**：分裂时叶子链表维护、内部节点 child parent 更新、递归向上插入/合并的边界条件处理。

2. **页面管理**：`pin/unpin` 的正确配对是缓冲区管理的基础。每一个 `fetch_node` / `create_node` 都需要在正确的位置 `unpin_page`，泄漏会导致系统死锁。

3. **并发控制**：即使是粗粒度锁，也需要注意锁的粒度和位置。锁应该放在公共入口函数，内部辅助函数不需要重复加锁。

4. **调试方法论**：
   - `find_child` 失败 → 检查 parent 指针（0 vs -1）
   - 死循环/卡死 → 检查 unpin 是否漏掉某个路径
   - 结构检查失败 → 检查 `maintain_parent` 是否在合适的位置被调用
   - 并发失败 → 检查锁的覆盖范围

### 关键 Bug 速查

| 症状 | 根因 | 修复位置 |
|------|------|----------|
| `find_child` 断言失败 | parent 被 memset 为 0 | `insert_into_parent`: `set_parent_page_no(IX_NO_PAGE)` |
| 电脑卡死 | adjust_root 漏 unpin | `coalesce_or_redistribute`: 手动 unpin |
| key 不匹配 | 没调 `maintain_parent` | `insert_entry` / `delete_entry` |
| 段错误 | `upper_bound` 返回 0 | `internal_lookup`: 加 if 保护 |
| 数据丢失（并发） | 无锁 | 三个入口函数加 `scoped_lock` |

### 文件修改清单

| 文件 | 修改内容 | 是否仅修改 TODO |
|------|---------|-----------------|
| `src/index/ix_index_handle.cpp` | 20 个 TODO 实现 + 5 处 bug 修复 | 是 |
| `src/system/sm_manager.cpp` | 3 个 TODO 实现（create_index, drop_index ×2） | 是 |
| `src/index/ix_index_handle.h` | 无修改 | N/A |
