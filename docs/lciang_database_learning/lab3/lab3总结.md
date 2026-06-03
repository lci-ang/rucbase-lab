# Lab3 查询执行实验报告

---

## 一、模块介绍

Lab3 实现 RucBase 教学数据库的**查询执行模块**，将 SQL 语句转化为对底层存储引擎的物理操作。

### 模块组成

| 类 | 作用 | 文件位置 |
|----|------|----------|
| `SmManager` | DDL 操作：open_db / close_db / drop_table / create_index / show_tables | `src/system/sm_manager.cpp` |
| `SeqScanExecutor` | 顺序扫描表记录，按条件过滤 | `src/execution/executor_seq_scan.h` |
| `ProjectionExecutor` | 从子执行器提取指定列，构造投影结果 | `src/execution/executor_projection.h` |
| `InsertExecutor` | 插入记录并维护所有索引 | `src/execution/executor_insert.h` |
| `DeleteExecutor` | 删除记录并维护所有索引 | `src/execution/executor_delete.h` |
| `UpdateExecutor` | 更新记录：删旧索引→删旧记录→插新记录→插新索引 | `src/execution/executor_update.h` |
| `NestedLoopJoinExecutor` | 嵌套循环连接两张表 | `src/execution/executor_nestedloop_join.h` |
| `IndexScanExecutor` | 利用 B+ 树索引范围扫描（bonus） | `src/execution/executor_index_scan.h` |
| `SortExecutor` | 物化+排序结果集（bonus） | `src/execution/execution_sort.h` |

### 执行流程

```
SQL 字符串
  → Parser (flex/bison 词法语法分析)
  → Analyze (语义分析，填充表名/列名/条件)
  → Planner (生成执行计划树)
  → Optimizer (选择最优计划)
  → Portal (构建执行器树并驱动执行)
  → 执行器调用 Record/Index/Storage 层
  → 输出结果
```

### 执行器迭代器接口

所有执行器继承 `AbstractExecutor`，实现统一的迭代器接口：

```cpp
virtual void beginTuple() = 0;   // 定位到第一条记录
virtual void nextTuple() = 0;    // 推进到下一条记录
virtual std::unique_ptr<RmRecord> Next() = 0;  // 返回当前记录
virtual bool is_end() const = 0; // 是否遍历完毕
virtual size_t tupleLen() const = 0;  // 记录长度
virtual const std::vector<ColMeta>& cols() const = 0;  // 列元数据
```

---

## 二、知识背景与相关技术

### 2.1 Volcano 迭代器模型

RucBase 采用经典的 **Volcano（火山）模型**：每个查询算子实现为一个迭代器，通过 `beginTuple()` / `nextTuple()` / `Next()` 接口按需拉取记录。上层算子调用下层算子的接口获取数据，形成一棵执行器树。

优点：流水线式执行，不需要物化全部中间结果。

### 2.2 条件求值

WHERE 子句中的条件（`Condition`）包含：
- `lhs_col`：左操作数（列名）
- `rhs_val` / `rhs_col`：右操作数（常量或另一列）
- `op`：比较运算符（EQ / NE / LT / GT / LE / GE）

`Value` 结构体在 analyze 阶段调用 `init_raw()` 将常量序列化为原始字节，存储在 `raw` 字段中。执行器直接用 `memcmp` 比较原始字节。

### 2.3 记录与字段定位

`RmRecord` 是原始字节缓冲区。字段通过 `ColMeta.offset` 定位：
```cpp
char *field_data = record->data + col_meta.offset;
```

### 2.4 索引扫描

`IndexScanExecutor` 利用 Lab2 实现的 B+ 树，通过 `lower_bound` / `upper_bound` 确定扫描范围，用 `IxScan` 迭代器遍历叶子节点。

### 2.5 DDL 操作

- `open_db`：读取 `db.meta` 元数据文件，打开所有表的记录文件和索引文件
- `close_db`：刷写元数据，关闭所有文件句柄
- `drop_table`：关闭记录文件，销毁记录文件和所有索引文件，更新元数据

---

## 三、实验内容与任务要求

### 任务1：DDL 操作（3 个 TODO）

实现 `SmManager` 的三个函数：
- `open_db`：打开数据库
- `close_db`：关闭数据库
- `drop_table`：删除表及其所有索引

### 任务2：顺序扫描与投影（6 个方法 + 辅助函数）

- `SeqScanExecutor::beginTuple` / `nextTuple` / `Next` / `is_end`
- `evaluate_condition` 辅助函数
- `ProjectionExecutor::beginTuple` / `nextTuple` / `Next` / `is_end`

### 任务3：插入、删除、更新（3 个方法）

- `InsertExecutor::Next`：插入记录 + 维护索引
- `DeleteExecutor::Next`：删除记录 + 维护索引
- `UpdateExecutor::Next`：删旧→插新 + 维护索引

### 任务4：嵌套循环连接（3 个方法 + 辅助函数）

- `NestedLoopJoinExecutor::beginTuple` / `nextTuple` / `Next` / `is_end`
- `evaluate_condition` 辅助函数

### 任务5（bonus）：排序与索引扫描

- `SortExecutor::beginTuple` / `nextTuple` / `Next` / `is_end`
- `IndexScanExecutor::beginTuple` / `nextTuple` / `Next` / `is_end`

### 计分标准

| 测试 | 内容 | 分值 |
|------|------|------|
| Test1 | DDL（建表/删表/建索引） | 25 |
| Test2 | 单表插入 + 条件查询 | 15 |
| Test3 | 单表更新 + 条件查询 | 15 |
| Test4 | 单表删除 + 条件查询 | 15 |
| Test5 | 连接查询（NLJ） | 30 |
| **总计** | | **100** |

---

## 四、实验步骤与具体实现

### 4.1 DDL 操作

#### open_db

```cpp
void SmManager::open_db(const std::string& db_name) {
    // ⚠️ 必须用完整路径读取 db.meta，否则从 build/ 目录读不到
    std::string meta_path = db_name + "/" + DB_META_NAME;
    std::ifstream ifs(meta_path);
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
    if (chdir(db_name.c_str()) < 0) throw UnixError();
}
```

#### close_db

```cpp
void SmManager::close_db() {
    flush_meta();
    for (auto &[name, fh] : fhs_) rm_manager_->close_file(fh.get());
    for (auto &[name, ih] : ihs_) ix_manager_->close_index(ih.get());
    fhs_.clear();
    ihs_.clear();
    if (chdir("..") < 0) throw UnixError();
}
```

#### drop_table

```cpp
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    rm_manager_->close_file(fhs_.at(tab_name).get());
    rm_manager_->destroy_file(tab_name);
    for (auto &index : tab.indexes) ix_manager_->destroy_index(tab_name, index.cols);
    db_.tabs_.erase(tab_name);
    fhs_.erase(tab_name);
    flush_meta();
}
```

### 4.2 条件求值辅助函数

```cpp
bool evaluate_condition(const Condition &cond, RmRecord *record) {
    auto lhs_col = get_col(cols_, cond.lhs_col);
    char *lhs_data = record->data + lhs_col->offset;
    char *rhs_data = nullptr;
    if (cond.is_rhs_val) {
        // ⚠️ cond.rhs_val.raw 已在 analyze 阶段初始化，不能复制后再调用 init_raw()
        rhs_data = cond.rhs_val.raw->data;
    } else {
        auto rhs_col = get_col(cols_, cond.rhs_col);
        rhs_data = record->data + rhs_col->offset;
    }
    int cmp = 0;
    if (lhs_col->type == TYPE_INT) cmp = *(int *)lhs_data - *(int *)rhs_data;
    else if (lhs_col->type == TYPE_FLOAT) {
        float l = *(float *)lhs_data, r = *(float *)rhs_data;
        cmp = (l < r) ? -1 : ((l > r) ? 1 : 0);
    } else if (lhs_col->type == TYPE_STRING) cmp = memcmp(lhs_data, rhs_data, lhs_col->len);
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

### 4.3 顺序扫描

```cpp
void beginTuple() override {
    scan_ = std::make_unique<RmScan>(fh_);
    while (!scan_->is_end()) {
        rid_ = scan_->rid();
        auto record = fh_->get_record(rid_, context_);
        bool match = true;
        for (auto &cond : fed_conds_) {
            if (!evaluate_condition(cond, record.get())) { match = false; break; }
        }
        if (match) return;
        scan_->next();
    }
}
```

`nextTuple` 从 `scan_->next()` 开始，逻辑相同。

### 4.4 投影

```cpp
void beginTuple() override { prev_->beginTuple(); }
void nextTuple() override { prev_->nextTuple(); }
std::unique_ptr<RmRecord> Next() override {
    auto &prev_cols = prev_->cols();
    auto &prev_rec = prev_->Next();
    auto rec = std::make_unique<RmRecord>(len_);
    for (size_t i = 0; i < sel_idxs_.size(); i++) {
        auto &col = prev_cols[sel_idxs_[i]];
        memcpy(rec->data + sel_offsets_[i], prev_rec->data + col.offset, col.len);
    }
    return rec;
}
```

### 4.5 插入

```cpp
std::unique_ptr<RmRecord> Next() override {
    RmRecord rec(fh_->get_file_hdr().record_size);
    for (size_t i = 0; i < values_.size(); i++) {
        auto &col = tab_.cols[i];
        memcpy(rec.data + col.offset, values_[i].raw->data, col.len);
    }
    rid_ = fh_->insert_record(rec.data, context_);
    // 维护所有索引
    for (auto &index : tab_.indexes) {
        auto ih = sm_manager_->ihs_.at(get_index_name(...)).get();
        char *key = new char[index.col_tot_len];
        // 从记录中提取索引列的值
        ih->insert_entry(key, rid_, nullptr);
        delete[] key;
    }
    return nullptr;
}
```

### 4.6 删除

```cpp
std::unique_ptr<RmRecord> Next() override {
    for (auto &rid : rids_) {
        auto record = fh_->get_record(rid, context_);
        // 先删索引
        for (auto &index : tab_.indexes) {
            char *key = new char[index.col_tot_len];
            // 从记录中提取索引列的值
            ih->delete_entry(key, nullptr);
            delete[] key;
        }
        // 再删记录
        fh_->delete_record(rid, context_);
    }
    return nullptr;
}
```

### 4.7 更新

```cpp
std::unique_ptr<RmRecord> Next() override {
    for (auto &rid : rids_) {
        auto old_record = fh_->get_record(rid, context_);
        RmRecord new_record(old_record->size);
        memcpy(new_record.data, old_record->data, old_record->size);
        // 修改字段
        for (auto &clause : set_clauses_) {
            auto col = tab_.get_col(clause.lhs.col_name);
            memcpy(new_record.data + col->offset, clause.rhs.raw->data, col->len);
        }
        // 删旧索引 → 删旧记录 → 插新记录 → 插新索引
        for (auto &index : tab_.indexes) { /* 删除旧键 */ }
        fh_->delete_record(rid, context_);
        rid_ = fh_->insert_record(new_record.data, context_);
        for (auto &index : tab_.indexes) { /* 插入新键 */ }
    }
    return nullptr;
}
```

### 4.8 嵌套循环连接

```cpp
void beginTuple() override {
    left_->beginTuple();
    right_->beginTuple();
    while (!left_->is_end()) {
        while (!right_->is_end()) {
            auto left_rec = left_->Next();
            auto right_rec = right_->Next();
            // 合并记录：[left_fields | right_fields]
            auto combined = merge_records(left_rec, right_rec);
            if (evaluate_all_conds(combined)) return;
            right_->nextTuple();
        }
        right_->beginTuple();  // 重置右表
        left_->nextTuple();
    }
    isend = true;
}
```

### 4.9 索引扫描（bonus）

```cpp
void beginTuple() override {
    // 在条件中找索引列，确定扫描范围
    for (auto &cond : fed_conds_) {
        if (cond.lhs_col.col_name == index_col_name) {
            lower = ih->lower_bound(cond.rhs_val.raw->data);
            upper = ih->upper_bound(cond.rhs_val.raw->data);
            break;
        }
    }
    scan_ = std::make_unique<IxScan>(ih, lower, upper, bpm);
    while (!scan_->is_end()) {
        // 验证非索引列条件
        if (evaluate_all_conds(record)) return;
        scan_->next();
    }
}
```

---

## 五、测试与性能分析

### 5.1 测试结果

```bash
cd src/test/query
python3 query_unit_test.py basic_query_test1.sql  # 25/25
python3 query_unit_test.py basic_query_test2.sql  # 15/15
python3 query_unit_test.py basic_query_test3.sql  # 15/15
python3 query_unit_test.py basic_query_test4.sql  # 15/15
python3 query_unit_test.py basic_query_test5.sql  # 30/30
```

**全部 5 个测试通过，总分 100/100。**

### 5.2 测试覆盖

| 测试 | 覆盖的执行器 | 关键验证点 |
|------|------------|-----------|
| Test1 (25分) | DDL | create/drop table, create index, show tables |
| Test2 (15分) | SeqScan + Projection | 单表条件查询，多条件 AND，列不存在报错 |
| Test3 (15分) | Update + SeqScan | 更新字段后查询验证，字符串溢出检查 |
| Test4 (15分) | Delete + SeqScan | 删除后查询验证，组合条件删除 |
| Test5 (30分) | NLJ + Projection | 笛卡尔积，等值连接，两表字段拼接 |

### 5.3 性能特征

- Test1-4：单表操作，毫秒级完成
- Test5：NLJ 复杂度 O(M×N)，本测试数据量小（3×16=48 对），仍为毫秒级
- 大数据量场景下 NLJ 会成为瓶颈，需要 Hash Join 或 Sort-Merge Join 优化

---

## 六、遇到问题与解决方案

### 问题1：服务器启动后 100% CPU 卡死

**现象**：运行 `query_unit_test.py` 后虚拟机完全无响应。用 `ps` 查看 rmdb 进程 CPU 占用 100%。

**排查过程**：
1. `strace` 显示无 `write` 系统调用 → 服务器卡在 `main()` 之前或初始化阶段
2. `script -q -c "./bin/rmdb ..."` 强制伪终端输出 → banner 正常打印，说明 `main()` 已执行
3. `ss -tlnp | grep 8765` → 端口未监听 → 卡在 `start_server()` 之前
4. 检查 `open_db` 发现 `ifs >> db_` 读取失败

**根因**：`open_db` 用 `DB_META_NAME`（即 `"db.meta"`）从当前目录读取，但 `create_db` 将 `db.meta` 写在了数据库目录 `query_test_db/` 内。ifstream 打开失败后，`operator>>` 读到未初始化的 `n`（`size_t` 类型），`for (size_t i = 0; i < n; i++)` 死循环。

**修复**：`open_db` 中用 `db_name + "/" + DB_META_NAME` 完整路径读取。

### 问题2：SELECT 条件查询崩溃（assertion `raw == nullptr`）

**现象**：Test2 执行 `select * from student where id >= 1` 时服务器 SIGABRT 崩溃。

**根因**：`evaluate_condition` 中复制 `cond.rhs_val` 后调用 `init_raw()`。但 `cond.rhs_val.raw` 已在 `analyze.cpp:147` 的 `check_clause()` 中初始化。复制操作拷贝了 `shared_ptr<RmRecord>`，`raw` 不为 null，`init_raw` 断言 `raw == nullptr` 失败。

**修复**：所有 `evaluate_condition` 和 `beginTuple` 中，直接使用 `cond.rhs_val.raw->data`，不复制 Value、不调用 `init_raw`。

涉及文件：
- `executor_seq_scan.h` — `evaluate_condition`
- `executor_index_scan.h` — `evaluate_condition` + `beginTuple`
- `executor_nestedloop_join.h` — `evaluate_condition`
- `executor_update.h` — `Next`

### 问题3：Test2-4 全部返回 "Error. Stopping"

**现象**：Test2、3、4 的 `query_test` 客户端返回非零退出码。

**排查**：手动运行 `./bin/query_test ...` 后检查服务器日志，发现都是 `init_raw` 断言失败导致的崩溃，属于问题2 的连锁反应。修复问题2 后全部通过。

---

## 七、实验总结

### 核心收获

1. **Volcano 迭代器模型**：理解了自顶向下的拉取式执行模型。每个执行器只需实现 `beginTuple` / `nextTuple` / `Next` / `is_end` 四个接口，上层算子通过这些接口按需获取数据。

2. **分析阶段与执行阶段的职责分离**：`Value.init_raw()` 在 analyze 阶段调用一次，将常量序列化为原始字节。执行阶段直接用 `raw->data` 进行 `memcmp` 比较。理解这个分工是避免重复初始化 bug 的关键。

3. **路径依赖问题**：`open_db` 中的相对路径 `DB_META_NAME` 在 `query_unit_test.py` 的 `chdir` 上下文中失效。教训：涉及文件 I/O 时，永远用完整路径或确认当前工作目录。

4. **调试方法论**：
   - 100% CPU → `strace -e write` 确认有无输出 → 缩小卡死范围
   - `script -q -c "..."` 强制伪终端绕过 stdout 缓冲
   - `ss -tlnp` 检查端口监听状态确认服务器是否就绪
   - GDB `break main` + `run` 确认程序入口

### Bug 速查表

| 症状 | 根因 | 修复位置 |
|------|------|----------|
| 服务器 100% CPU 卡死 | `open_db` 读不到 `db.meta` → 未初始化变量死循环 | `sm_manager.cpp`: 用完整路径 |
| `assert(raw == nullptr)` 崩溃 | `evaluate_condition` 复制 Value 后重复调用 `init_raw` | 4 个 executor 文件：直接用 `cond.rhs_val.raw->data` |
| 字符串溢出异常 | INSERT 时字段长度超过列定义 | `executor_insert.h`：analyze 阶段已检查，execute 阶段无需处理 |

### 文件修改清单

| 文件 | 修改内容 | 是否仅修改 TODO |
|------|---------|-----------------|
| `src/system/sm_manager.cpp` | open_db / close_db / drop_table / create_index / drop_index | 是 |
| `src/execution/executor_seq_scan.h` | beginTuple / nextTuple / Next / is_end / evaluate_condition | 是 |
| `src/execution/executor_projection.h` | beginTuple / nextTuple / Next / is_end | 是 |
| `src/execution/executor_insert.h` | Next | 是 |
| `src/execution/executor_delete.h` | Next | 是 |
| `src/execution/executor_update.h` | Next | 是 |
| `src/execution/executor_nestedloop_join.h` | beginTuple / nextTuple / Next / is_end / evaluate_condition | 是 |
| `src/execution/executor_index_scan.h` | beginTuple / nextTuple / Next / is_end / evaluate_condition | 是（bonus） |
| `src/execution/execution_sort.h` | beginTuple / nextTuple / Next / is_end + 成员变量 | 是（bonus） |
