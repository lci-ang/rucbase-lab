# Lab3 查询执行：完整学习与实现指南

> 本指南覆盖 RucBase Lab3（Query Execution）的全部实现内容。目标是让你彻底理解每个执行器的工作原理，然后自己动手把代码写进源文件。
>
> 文档结构：先教原理 → 再告诉你该改哪个文件 → 给你可直接复制的完整代码 → 最后给你验证方法。

---

## 目录

- [1. 你需要先搞懂的基础知识](#1-你需要先搞懂的基础知识)
- [2. DDL 元数据管理：3 个 TODO](#2-ddl-元数据管理3-个-todo)
- [3. 条件求值辅助函数（共享）](#3-条件求值辅助函数共享)
- [4. SeqScanExecutor：3 TODO + 1 is_end](#4-seqscanexecutor3-todo--1-is_end)
- [5. ProjectionExecutor：3 TODO + 1 is_end](#5-projectionexecutor3-todo--1-is_end)
- [6. NestedLoopJoinExecutor：3 TODO + 1 is_end](#6-nestedloopjoinexecutor3-todo--1-is_end)
- [7. DeleteExecutor：1 TODO](#7-deleteexecutor1-todo)
- [8. UpdateExecutor：1 TODO](#8-updateexecutor1-todo)
- [9. SortExecutor：3 TODO（加分项）](#9-sortexecutor3-todo加分项)
- [10. IndexScanExecutor：3 TODO（加分项）](#10-indexscanexecutor3-todo加分项)
- [11. 完整测试流程](#11-完整测试流程)
- [12. 调试技巧](#12-调试技巧)
- [附录：各函数速查表](#附录各函数速查表)

---

## 1. 你需要先搞懂的基础知识

### 1.1 算子拉取模型（Volcano/Iterator Model）

RucBase 的查询执行采用经典的 **Volcano 模型**。每个执行器实现相同的接口，数据从叶子节点逐条"拉取"到根节点。

驱动循环位于 `execution_manager.cpp` 第 156 行：

```cpp
// 遍历执行器树，逐条获取元组并输出
for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
    auto Tuple = executorTreeRoot->Next();
    // ... 格式化并输出 Tuple 的各列 ...
}
```

四个方法的语义：

| 方法 | 作用 | 调用时机 |
|------|------|----------|
| `beginTuple()` | 定位到第一条元组 | 循环开始前，只调用一次 |
| `is_end()` | 判断是否已遍历完所有元组 | 每次循环条件判断 |
| `Next()` | 返回当前指向的元组数据 | 循环体内，获取实际数据 |
| `nextTuple()` | 推进到下一条元组 | 每次循环体执行完后 |

**最关键的陷阱**：`AbstractExecutor::is_end()` 默认返回 `true`：

```cpp
// executor_abstract.h 第 39 行
virtual bool is_end() const { return true; };
```

如果你不在自己的执行器中重写 `is_end()`，循环条件永远为 `false`，循环体一次都不执行。**这是 Lab3 中排名第一的 bug 来源**。

---

### 1.2 执行器树结构（Plan → Executor 映射）

| Plan 节点 | Executor | 说明 |
|-----------|----------|------|
| `ProjectionPlan` | `ProjectionExecutor` | 列投影 |
| `ScanPlan(T_SeqScan)` | `SeqScanExecutor` | 全表顺序扫描 |
| `ScanPlan(T_IndexScan)` | `IndexScanExecutor` | 索引扫描 |
| `JoinPlan(T_NestLoop)` | `NestedLoopJoinExecutor` | 嵌套循环连接 |
| `SortPlan` | `SortExecutor` | 排序 |
| `DMLPlan(T_Insert)` | `InsertExecutor` | 插入 |
| `DMLPlan(T_Delete)` | `DeleteExecutor` | 删除 |
| `DMLPlan(T_Update)` | `UpdateExecutor` | 更新 |

典型的 SELECT 执行器树：
```
ProjectionExecutor
  └── NestedLoopJoinExecutor (如果有 JOIN)
        ├── left_ child: SeqScanExecutor
        └── right_ child: SeqScanExecutor
```

---

### 1.3 DML 单次调用契约

`run_dml()` 只调用一次 `exec->Next()`。Portal 先用 scan 执行器收集所有匹配的 `Rid`，再传给 Delete/Update。因此 DML 的 `Next()` 必须在一次调用中处理所有 `rids_`。

---

### 1.4 RmRecord 内存模型

`data` 是堆上分配的字节缓冲区，按列偏移量存放各列值。拷贝构造做深拷贝。访问某列数据：`record->data + col.offset`。

---

### 1.5 RmScan 迭代模式

```cpp
auto scan = std::make_unique<RmScan>(fh_);
while (!scan->is_end()) {
    Rid rid = scan->rid();
    auto record = fh_->get_record(rid, context);
    scan->next();
}
```

---

### 1.6 条件求值语义

```cpp
struct Condition {
    TabCol lhs_col;    // 左操作列
    CompOp op;         // OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE
    bool is_rhs_val;   // true: 右值是常量; false: 右值是另一列
    TabCol rhs_col;    // 右操作列
    Value rhs_val;     // 右操作值
};
```

---

### 1.7 索引键构建模式

```cpp
char* key = new char[index.col_tot_len];
int offset = 0;
for (size_t i = 0; i < index.col_num; ++i) {
    memcpy(key + offset, rec.data + index.cols[i].offset, index.cols[i].len);
    offset += index.cols[i].len;
}
ih->insert_entry(key, rid, context_->txn_);
delete[] key;
```

---

### 1.8 核心数据结构速查

| 数据结构 | 定义位置 | 用途 |
|----------|----------|------|
| `TabCol` | `common/common.h` | 列标识：`tab_name` + `col_name` |
| `Value` | `common/common.h` | 常量值，`init_raw()` 序列化为原始字节 |
| `Condition` | `common/common.h` | 比较条件 |
| `SetClause` | `common/common.h` | UPDATE 的 SET 子句 |
| `ColMeta` | `system/sm_meta.h` | 列元数据：类型、长度、偏移量 |
| `IndexMeta` | `system/sm_meta.h` | 索引元数据 |
| `TabMeta` | `system/sm_meta.h` | 表元数据 |
| `Rid` | `defs.h` | 记录位置：`(page_no, slot_no)` |
| `RmRecord` | `record/rm_defs.h` | 记录数据：`data` + `size` |

---

## 2. DDL 元数据管理：3 个 TODO

### TODO 2.1: open_db()

**位置**：`sm_manager.cpp` 第 87-89 行

**直接复制到源文件的代码**：

```cpp
void SmManager::open_db(const std::string& db_name) {
    // 读取数据库元数据
    std::ifstream ifs(DB_META_NAME);
    ifs >> db_;
    // 打开每张表的记录文件和索引文件
    for (auto &entry : db_.tabs_) {
        auto &tab_name = entry.first;
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
        for (auto &index : entry.second.indexes) {
            auto index_name = ix_manager_->get_index_name(tab_name, index.cols);
            ihs_.emplace(index_name, ix_manager_->open_index(tab_name, index.cols));
        }
    }
    // 进入数据库目录（必须在打开文件之后）
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
}
```

**为什么这么写**：必须先读元数据再打开文件，因为 `open_file` 需要在当前目录找到文件。`index_name` 必须通过 `ix_manager_->get_index_name()` 获取。

---

### TODO 2.2: close_db()

**位置**：`sm_manager.cpp` 第 103-105 行

**直接复制到源文件的代码**：

```cpp
void SmManager::close_db() {
    flush_meta();
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    db_.tabs_.clear();
    fhs_.clear();
    ihs_.clear();
    if (chdir("..") < 0) {
        throw UnixError();
    }
}
```

**为什么这么写**：用 `.get()` 取裸指针，**不能用 `.release()`**。必须先 close 再 clear。

---

### TODO 2.3: drop_table()

**位置**：`sm_manager.cpp` 第 190-192 行

**直接复制到源文件的代码**：

```cpp
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    rm_manager_->close_file(fhs_.at(tab_name).get());
    rm_manager_->destroy_file(tab_name);
    for (auto &index : tab.indexes) {
        ix_manager_->destroy_index(tab_name, index.cols);
    }
    db_.tabs_.erase(tab_name);
    fhs_.erase(tab_name);
    flush_meta();
}
```

**为什么这么写**：必须先 close 再 destroy。close 会刷缓冲池和写文件头。

---

## 3. 条件求值辅助函数（共享）

> **⚠️ 非 TODO 必要修改**：源文件中没有此函数的占位符。需要在 SeqScan、IndexScan、NestedLoopJoin 三个执行器类的 `private` 区域手动添加此辅助函数。原因是框架没有提供通用的条件求值接口，每个执行器需要自行实现。

```cpp
bool evaluate_condition(const Condition &cond, RmRecord *record) {
    // 查找左操作列的元数据
    auto lhs_col = get_col(cols_, cond.lhs_col);
    char *lhs_data = record->data + lhs_col->offset;

    char *rhs_data = nullptr;
    Value rhs_val;
    if (cond.is_rhs_val) {
        // 右值是常量，必须调用 init_raw 序列化为原始字节
        rhs_val = cond.rhs_val;
        rhs_val.init_raw(lhs_col->len);
        rhs_data = rhs_val.raw->data;
    } else {
        // 右值是另一列
        auto rhs_col = get_col(cols_, cond.rhs_col);
        rhs_data = record->data + rhs_col->offset;
    }

    // 根据列类型比较
    int cmp = 0;
    if (lhs_col->type == TYPE_INT) {
        cmp = *(int *)lhs_data - *(int *)rhs_data;
    } else if (lhs_col->type == TYPE_FLOAT) {
        float l = *(float *)lhs_data, r = *(float *)rhs_data;
        cmp = (l < r) ? -1 : ((l > r) ? 1 : 0);
    } else if (lhs_col->type == TYPE_STRING) {
        cmp = memcmp(lhs_data, rhs_data, lhs_col->len);
    }

    switch (cond.op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
        default: return false;
    }
}
```

**三个陷阱**：
1. `rhs_val.init_raw(lhs_col->len)` 必须调用！否则 `raw` 为 null → segfault
2. 字符串用 `memcmp` + `lhs_col->len`，不能用 `strcmp`
3. `get_col()` 是 AbstractExecutor 的 protected 方法，直接调用即可

---

## 4. SeqScanExecutor：3 TODO + 1 is_end

**文件**：`executor_seq_scan.h`

### is_end() override（必须手动添加）

> **⚠️ 非 TODO 必要修改**：源文件中没有 `is_end()` 的占位符。基类 `AbstractExecutor::is_end()` 默认返回 `true`，不重写会导致驱动循环 `for(beginTuple(); !is_end(); nextTuple())` 永远不执行循环体。

```cpp
bool is_end() const override {
    return scan_->is_end();
}
```

### TODO 4.1: beginTuple()
**位置**：第 52-54 行

```cpp
void beginTuple() override {
    scan_ = std::make_unique<RmScan>(fh_);
    while (!scan_->is_end()) {
        rid_ = scan_->rid();
        auto record = fh_->get_record(rid_, context_);
        bool match = true;
        for (auto &cond : fed_conds_) {
            if (!evaluate_condition(cond, record.get())) {
                match = false;
                break;
            }
        }
        if (match) return;
        scan_->next();
    }
}
```

### TODO 4.2: nextTuple()
**位置**：第 60-62 行

```cpp
void nextTuple() override {
    scan_->next();
    while (!scan_->is_end()) {
        rid_ = scan_->rid();
        auto record = fh_->get_record(rid_, context_);
        bool match = true;
        for (auto &cond : fed_conds_) {
            if (!evaluate_condition(cond, record.get())) {
                match = false;
                break;
            }
        }
        if (match) return;
        scan_->next();
    }
}
```

### TODO 4.3: Next()
**位置**：第 69-71 行

```cpp
std::unique_ptr<RmRecord> Next() override {
    return fh_->get_record(rid_, context_);
}
```

---

## 5. ProjectionExecutor：3 TODO + 1 is_end

**文件**：`executor_projection.h`

### is_end() override（必须手动添加）

> **⚠️ 非 TODO 必要修改**：同 SeqScan，不重写 `is_end()` 会导致循环不执行。

```cpp
bool is_end() const override {
    return prev_->is_end();
}
```

### TODO 5.1: beginTuple()
**位置**：第 42 行

```cpp
void beginTuple() override {
    prev_->beginTuple();
}
```

### TODO 5.2: nextTuple()
**位置**：第 44 行

```cpp
void nextTuple() override {
    prev_->nextTuple();
}
```

### TODO 5.3: Next()
**位置**：第 46-48 行

```cpp
std::unique_ptr<RmRecord> Next() override {
    auto prev_record = prev_->Next();
    auto record = std::make_unique<RmRecord>(len_);
    for (size_t i = 0; i < sel_idxs_.size(); i++) {
        auto &src_col = prev_->cols()[sel_idxs_[i]];
        memcpy(record->data + cols_[i].offset, prev_record->data + src_col.offset, src_col.len);
    }
    return record;
}
```

**陷阱**：源偏移用 `prev_->cols()[sel_idxs_[i]].offset`，目标偏移用 `cols_[i].offset`，不要搞混。

---

## 6. NestedLoopJoinExecutor：3 TODO + 1 is_end

**文件**：`executor_nestedloop_join.h`

### is_end() override（必须手动添加）

> **⚠️ 非 TODO 必要修改**：同 SeqScan，不重写 `is_end()` 会导致循环不执行。NLJ 使用成员变量 `isend` 而非委托给子执行器。

```cpp
bool is_end() const override {
    return isend;
}
```

### TODO 6.1: beginTuple()
**位置**：第 46-48 行

```cpp
void beginTuple() override {
    left_->beginTuple();
    if (left_->is_end()) {
        isend = true;
        return;
    }
    while (!left_->is_end()) {
        right_->beginTuple();
        while (!right_->is_end()) {
            auto left_rec = left_->Next();
            auto right_rec = right_->Next();
            auto combined = std::make_unique<RmRecord>(len_);
            memcpy(combined->data, left_rec->data, left_->tupleLen());
            memcpy(combined->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
            bool match = true;
            for (auto &cond : fed_conds_) {
                if (!evaluate_condition(cond, combined.get())) {
                    match = false;
                    break;
                }
            }
            if (match) return;
            right_->nextTuple();
        }
        left_->nextTuple();
    }
    isend = true;
}
```

### TODO 6.2: nextTuple()
**位置**：第 50-52 行

```cpp
void nextTuple() override {
    right_->nextTuple();
    while (!left_->is_end()) {
        while (!right_->is_end()) {
            auto left_rec = left_->Next();
            auto right_rec = right_->Next();
            auto combined = std::make_unique<RmRecord>(len_);
            memcpy(combined->data, left_rec->data, left_->tupleLen());
            memcpy(combined->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
            bool match = true;
            for (auto &cond : fed_conds_) {
                if (!evaluate_condition(cond, combined.get())) {
                    match = false;
                    break;
                }
            }
            if (match) return;
            right_->nextTuple();
        }
        left_->nextTuple();
        if (!left_->is_end()) {
            right_->beginTuple();
        }
    }
    isend = true;
}
```

### TODO 6.3: Next()
**位置**：第 54-56 行

```cpp
std::unique_ptr<RmRecord> Next() override {
    auto record = std::make_unique<RmRecord>(len_);
    auto left_rec = left_->Next();
    auto right_rec = right_->Next();
    memcpy(record->data, left_rec->data, left_->tupleLen());
    memcpy(record->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
    return record;
}
```

**为什么合并记录**：构造函数已将右列 offset 加上 `left_->tupleLen()`，合并后的记录布局与 `cols_` 一致，evaluate_condition 可直接用 `cols_` 定位字段。

---

## 7. DeleteExecutor：1 TODO

**文件**：`executor_delete.h`

### TODO 7.1: Next()
**位置**：第 39-41 行

```cpp
std::unique_ptr<RmRecord> Next() override {
    for (auto &rid : rids_) {
        auto record = fh_->get_record(rid, context_);
        // 先从索引删除（需要记录数据构建 key）
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            auto ih = sm_manager_->ihs_.at(
                sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)
            ).get();
            char *key = new char[index.col_tot_len];
            int offset = 0;
            for (size_t j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, record->data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }
            ih->delete_entry(key, context_->txn_);
            delete[] key;
        }
        // 再删除记录
        fh_->delete_record(rid, context_);
    }
    return nullptr;
}
```

---

## 8. UpdateExecutor：1 TODO

**文件**：`executor_update.h`

### TODO 8.1: Next()
**位置**：第 40-43 行

```cpp
std::unique_ptr<RmRecord> Next() override {
    for (auto &rid : rids_) {
        auto old_record = fh_->get_record(rid, context_);
        // 构建新记录
        RmRecord new_record(old_record->size);
        memcpy(new_record.data, old_record->data, old_record->size);
        for (auto &clause : set_clauses_) {
            auto col = tab_.get_col(clause.lhs.col_name);
            Value val = clause.rhs;
            val.init_raw(col->len);
            memcpy(new_record.data + col->offset, val.raw->data, col->len);
        }
        // 从索引删除旧键
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            auto ih = sm_manager_->ihs_.at(
                sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)
            ).get();
            char *old_key = new char[index.col_tot_len];
            int offset = 0;
            for (size_t j = 0; j < index.col_num; ++j) {
                memcpy(old_key + offset, old_record->data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }
            ih->delete_entry(old_key, context_->txn_);
            delete[] old_key;
        }
        // 删除旧记录，插入新记录
        fh_->delete_record(rid, context_);
        Rid new_rid = fh_->insert_record(new_record.data, context_);
        // 将新键插入索引
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            auto ih = sm_manager_->ihs_.at(
                sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)
            ).get();
            char *new_key = new char[index.col_tot_len];
            int offset = 0;
            for (size_t j = 0; j < index.col_num; ++j) {
                memcpy(new_key + offset, new_record.data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }
            ih->insert_entry(new_key, new_rid, context_->txn_);
            delete[] new_key;
        }
    }
    return nullptr;
}
```

**操作顺序**：删旧键 → 删旧记录 → 插新记录 → 插新键。因为 delete_record 使旧 Rid 失效，insert_record 返回新 Rid。

---

## 9. SortExecutor：3 TODO（加分项）

**文件**：`execution_sort.h`

### 需要额外添加的成员变量

> **⚠️ 非 TODO 必要修改**：源文件中没有这两个成员变量的占位符。`sorted_records_` 用于缓存所有排序后的记录，`current_idx_` 用于跟踪当前位置。必须在 `private` 部分手动添加。

```cpp
std::vector<std::unique_ptr<RmRecord>> sorted_records_;
size_t current_idx_ = 0;
```

### is_end() override（必须手动添加）

> **⚠️ 非 TODO 必要修改**：同 SeqScan，不重写 `is_end()` 会导致循环不执行。Sort 使用 `current_idx_` 判断是否遍历完。

```cpp
bool is_end() const override {
    return current_idx_ >= tuple_num;
}
```

### TODO 9.1: beginTuple()
**位置**：第 36-38 行

```cpp
void beginTuple() override {
    prev_->beginTuple();
    sorted_records_.clear();
    while (!prev_->is_end()) {
        sorted_records_.push_back(prev_->Next());
        prev_->nextTuple();
    }
    tuple_num = sorted_records_.size();
    if (tuple_num == 0) return;

    used_tuple.resize(tuple_num);
    for (size_t i = 0; i < tuple_num; i++) {
        used_tuple[i] = i;
    }

    std::sort(used_tuple.begin(), used_tuple.end(), [&](size_t a, size_t b) {
        char *data_a = sorted_records_[a]->data + cols_.offset;
        char *data_b = sorted_records_[b]->data + cols_.offset;
        if (cols_.type == TYPE_INT) {
            return is_desc_ ? *(int *)data_a > *(int *)data_b : *(int *)data_a < *(int *)data_b;
        } else if (cols_.type == TYPE_FLOAT) {
            return is_desc_ ? *(float *)data_a > *(float *)data_b : *(float *)data_a < *(float *)data_b;
        } else {
            int cmp = memcmp(data_a, data_b, cols_.len);
            return is_desc_ ? cmp > 0 : cmp < 0;
        }
    });

    current_idx_ = 0;
    current_tuple = std::move(sorted_records_[used_tuple[0]]);
}
```

### TODO 9.2: nextTuple()
**位置**：第 40-42 行

```cpp
void nextTuple() override {
    current_idx_++;
    if (current_idx_ < tuple_num) {
        current_tuple = std::move(sorted_records_[used_tuple[current_idx_]]);
    }
}
```

### TODO 9.3: Next()
**位置**：第 44-46 行

```cpp
std::unique_ptr<RmRecord> Next() override {
    return std::move(current_tuple);
}
```

---

## 10. IndexScanExecutor：3 TODO（加分项）

**文件**：`executor_index_scan.h`

### 需要额外添加的 evaluate_condition

> **⚠️ 非 TODO 必要修改**：源文件中没有此函数的占位符。与 SeqScan/NLJ 的版本不同，这里用 `tab_.get_col()` 而非 `get_col(cols_, ...)`。两者功能等价（因为 `cols_ = tab_.cols`），但 `tab_.get_col()` 返回非 const 迭代器，与源文件中其他地方的用法一致。

在 `private` 区域添加：

```cpp
bool evaluate_condition(const Condition &cond, RmRecord *record) {
    auto lhs_col = tab_.get_col(cond.lhs_col.col_name);
    char *lhs_data = record->data + lhs_col->offset;

    char *rhs_data = nullptr;
    Value rhs_val;
    if (cond.is_rhs_val) {
        rhs_val = cond.rhs_val;
        rhs_val.init_raw(lhs_col->len);
        rhs_data = rhs_val.raw->data;
    } else {
        auto rhs_col = tab_.get_col(cond.rhs_col.col_name);
        rhs_data = record->data + rhs_col->offset;
    }

    int cmp = 0;
    if (lhs_col->type == TYPE_INT) {
        cmp = *(int *)lhs_data - *(int *)rhs_data;
    } else if (lhs_col->type == TYPE_FLOAT) {
        float l = *(float *)lhs_data, r = *(float *)rhs_data;
        cmp = (l < r) ? -1 : ((l > r) ? 1 : 0);
    } else if (lhs_col->type == TYPE_STRING) {
        cmp = memcmp(lhs_data, rhs_data, lhs_col->len);
    }

    switch (cond.op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
        default: return false;
    }
}
```

### is_end() override（必须手动添加）

> **⚠️ 非 TODO 必要修改**：同 SeqScan，不重写 `is_end()` 会导致循环不执行。

```cpp
bool is_end() const override {
    return scan_->is_end();
}
```

### TODO 10.1: beginTuple()
**位置**：第 67-69 行

```cpp
void beginTuple() override {
    auto ih = sm_manager_->ihs_.at(
        sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)
    ).get();

    Iid lower = ih->leaf_begin();
    Iid upper = ih->leaf_end();

    // 从条件中提取索引列的等值条件
    for (auto &cond : fed_conds_) {
        if (cond.is_rhs_val && cond.op == OP_EQ &&
            cond.lhs_col.tab_name == tab_name_ &&
            tab_.is_col(cond.lhs_col.col_name)) {
            bool is_index_col = false;
            for (auto &idx_col : index_meta_.cols) {
                if (idx_col.name == cond.lhs_col.col_name) {
                    is_index_col = true;
                    break;
                }
            }
            if (is_index_col) {
                // 用单列长度构建搜索键
                Value val = cond.rhs_val;
                val.init_raw(index_meta_.cols[0].len);
                lower = ih->lower_bound(val.raw->data);
                upper = ih->upper_bound(val.raw->data);
                break;
            }
        }
    }

    scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());

    while (!scan_->is_end()) {
        rid_ = scan_->rid();
        auto record = fh_->get_record(rid_, context_);
        bool match = true;
        for (auto &cond : fed_conds_) {
            if (!evaluate_condition(cond, record.get())) {
                match = false;
                break;
            }
        }
        if (match) return;
        scan_->next();
    }
}
```

### TODO 10.2: nextTuple()
**位置**：第 71-72 行

```cpp
void nextTuple() override {
    scan_->next();
    while (!scan_->is_end()) {
        rid_ = scan_->rid();
        auto record = fh_->get_record(rid_, context_);
        bool match = true;
        for (auto &cond : fed_conds_) {
            if (!evaluate_condition(cond, record.get())) {
                match = false;
                break;
            }
        }
        if (match) return;
        scan_->next();
    }
}
```

### TODO 10.3: Next()
**位置**：第 75-77 行

```cpp
std::unique_ptr<RmRecord> Next() override {
    return fh_->get_record(rid_, context_);
}
```

---

## 11. 完整测试流程

```bash
# 构建项目
cd /home/lc/桌面/db/rucbase-lab/build
cmake .. && make -j$(nproc)

# 进入测试目录
cd /home/lc/桌面/db/rucbase-lab/src/test/query

# Test 1: DDL（25分）
python query_unit_test.py basic_query_test1.sql

# Test 2: INSERT + SELECT（15分）
python query_unit_test.py basic_query_test2.sql

# Test 3: UPDATE + SELECT（15分）
python query_unit_test.py basic_query_test3.sql

# Test 4: DELETE + SELECT（15分）
python query_unit_test.py basic_query_test4.sql

# Test 5: JOIN（30分）
python query_unit_test.py basic_query_test5.sql

# 全量测试
python query_test_basic.py
```

| 测试 | 分值 | 测试要点 |
|------|------|----------|
| Test 1: DDL | 25分 | CREATE/DROP TABLE, SHOW TABLES, CREATE INDEX |
| Test 2: INSERT + SELECT | 15分 | 单表插入与条件查询（=, >=, AND） |
| Test 3: UPDATE + SELECT | 15分 | 单表更新与条件查询验证 |
| Test 4: DELETE + SELECT | 15分 | 单表删除与条件查询验证 |
| Test 5: JOIN | 30分 | 多表连接查询（cross join + equi-join） |
| **合计** | **100分** | |

---

## 12. 调试技巧

### 12.1 常见编译错误

- **"is_end was not declared"**: is_end() override 没有放在类的 `public:` 区域
- **"evaluate_condition is not a member"**: 辅助函数定义在类外面了，必须放在类内部 private 区域

### 12.2 常见运行时错误

- **空输出**: 忘记重写 is_end()。基类默认返回 true
- **段错误**: evaluate_condition 中忘记调用 `rhs_val.init_raw(col_len)`
- **行数不对**: beginTuple 没定位到第一条匹配，或 nextTuple 跳过了记录

### 12.3 调试策略

- 在 beginTuple/nextTuple 中添加 printf 追踪
- 用 output.txt 对比期望输出
- 逐个测试定位问题算子

---

## 附录：各函数速查表

| 模块 | 函数 | 位置 | 核心操作 |
|------|------|------|----------|
| DDL | `open_db` | sm_manager.cpp:87 | 读元数据，打开文件，chdir |
| DDL | `close_db` | sm_manager.cpp:103 | flush，关闭文件，清空，chdir |
| DDL | `drop_table` | sm_manager.cpp:190 | 关闭+销毁记录，销毁索引 |
| Scan | `is_end` | executor_seq_scan.h（新增） | 委托 scan_->is_end() |
| Scan | `beginTuple` | executor_seq_scan.h:52 | 创建 RmScan，找第一条匹配 |
| Scan | `nextTuple` | executor_seq_scan.h:60 | 推进扫描 |
| Scan | `Next` | executor_seq_scan.h:69 | 返回当前记录 |
| Proj | `is_end` | executor_projection.h（新增） | 委托 prev_->is_end() |
| Proj | `beginTuple` | executor_projection.h:42 | 委托 prev_->beginTuple() |
| Proj | `nextTuple` | executor_projection.h:44 | 委托 prev_->nextTuple() |
| Proj | `Next` | executor_projection.h:46 | 复制选定列到新 RmRecord |
| Join | `is_end` | executor_nestedloop_join.h（新增） | 返回 isend 标志 |
| Join | `beginTuple` | executor_nestedloop_join.h:46 | 嵌套循环，找第一对匹配 |
| Join | `nextTuple` | executor_nestedloop_join.h:50 | 推进右/重置右/推进左 |
| Join | `Next` | executor_nestedloop_join.h:54 | 合并左右记录 |
| Delete | `Next` | executor_delete.h:39 | 遍历 rid，删索引+删记录 |
| Update | `Next` | executor_update.h:40 | 构建新记录，更新索引 |
| Sort | `is_end` | execution_sort.h（新增） | current_idx_ >= tuple_num |
| Sort | `beginTuple` | execution_sort.h:36 | 物化+排序 |
| Sort | `nextTuple` | execution_sort.h:40 | 推进排序位置 |
| Sort | `Next` | execution_sort.h:44 | 返回当前元组 |
| IScan | `is_end` | executor_index_scan.h（新增） | 委托 scan_->is_end() |
| IScan | `beginTuple` | executor_index_scan.h:67 | IxScan 定位范围+验证 |
| IScan | `nextTuple` | executor_index_scan.h:71 | 推进 IxScan+验证 |
| IScan | `Next` | executor_index_scan.h:75 | 返回当前记录 |

---

## 推荐实现顺序

1. **sm_manager.cpp**（open_db, close_db, drop_table）— 所有测试的前提
2. **SeqScanExecutor**（is_end + beginTuple + nextTuple + Next）— 所有查询的基础
3. **ProjectionExecutor**（is_end + beginTuple + nextTuple + Next）— SELECT 必需
4. **DeleteExecutor**（Next）— Test 4
5. **UpdateExecutor**（Next）— Test 3
6. **NestedLoopJoinExecutor**（is_end + beginTuple + nextTuple + Next）— Test 5
7. **SortExecutor** — 加分项
8. **IndexScanExecutor** — 加分项

每个 executor 内部顺序：`is_end()` → `beginTuple()` → `nextTuple()` → `Next()`
