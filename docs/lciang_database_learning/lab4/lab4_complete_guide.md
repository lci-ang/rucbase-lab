# Lab4 并发控制：完整学习与实现指南

> 本指南涵盖事务管理器（3 个 TODO）、锁管理器（7 个 TODO）、两阶段封锁协议集成（4 处修改），以及完整的测试流程和调试技巧。

---

## 目录

1. [你需要先搞懂的基础知识](#1-你需要先搞懂的基础知识)
2. [事务管理器 TransactionManager（3 个 TODO）](#2-事务管理器-transactionmanager3-个-todo)
3. [锁管理器 LockManager（7 个 TODO）](#3-锁管理器-lockmanager7-个-todo)
4. [两阶段封锁协议集成（4 处修改）](#4-两阶段封锁协议集成4-处修改)
5. [完整测试流程](#5-完整测试流程)
6. [调试技巧](#6-调试技巧)
7. [常见错误与解决方案](#7-常见错误与解决方案)
8. [附录：各函数速查表](#附录各函数速查表)

---

## 1. 你需要先搞懂的基础知识

### 1.1 事务的 ACID 特性

| 特性 | 含义 | Lab4 中谁负责 |
|---|---|---|
| **原子性** | 事务要么全部做完，要么全部回滚 | `TransactionManager::abort()` |
| **一致性** | 事务执行前后，数据库都满足完整性约束 | 由上层业务逻辑保证 |
| **隔离性** | 并发事务之间互不干扰 | **LockManager + 2PL 协议**（本 Lab 核心） |
| **持久性** | 事务一旦提交，其结果永久保存 | `LogManager` |

### 1.2 两阶段封锁协议 (2PL)

```
事务的生命周期分为两个阶段：

  ┌─────────────┐      ┌──────────────┐
  │  GROWING     │ ──→  │  SHRINKING    │
  │  （增长阶段） │      │  （收缩阶段）  │
  └─────────────┘      └──────────────┘
   只能获取锁            只能释放锁
   不能释放锁            不能获取新锁
```

**关键约束**：一旦事务释放了任何一个锁，就进入收缩阶段，此后不能再获取任何新锁。

### 1.3 锁的类型

#### 行级锁（Record Lock）

| 锁模式 | 缩写 | 含义 |
|---|---|---|
| 共享锁 | S | 读锁，多个事务可以同时持有 |
| 排他锁 | X | 写锁，独占访问 |

#### 表级锁（Table Lock）

| 锁模式 | 缩写 | 含义 |
|---|---|---|
| 意向共享锁 | IS | 声明"我要读这张表里的某些行" |
| 意向排他锁 | IX | 声明"我要写这张表里的某些行" |
| 共享锁 | S | 整张表只读 |
| 排他锁 | X | 整张表独占 |
| 共享意向排他锁 | SIX | 先 S 再 IX 的合并 |

#### 锁兼容矩阵

| Request \ Held | IS | IX | S | X | SIX |
|---|---|---|---|---|---|
| **IS** | Yes | Yes | Yes | No | No |
| **IX** | Yes | Yes | No | No | No |
| **S** | Yes | No | Yes | No | No |
| **X** | No | No | No | No | No |
| **SIX** | No | No | No | No | No |

### 1.4 死锁预防 — No-Wait 策略

申请锁时发现冲突 → 不等待，直接抛出 `TransactionAbortException`，abort 当前事务。

### 1.5 事务状态机

```
DEFAULT ──begin()──→ GROWING ──commit()──→ COMMITTED
                        │
                        │ abort()
                        ↓
                     ABORTED
```

### 1.6 回滚机制

`write_set_` 记录所有写操作。abort 时**逆序**回滚：
- `INSERT_TUPLE` → `delete_record`（插入的要删掉）
- `DELETE_TUPLE` → `insert_record`（删除的要恢复）
- `UPDATE_TUPLE` → `update_record`（更新的要恢复旧值）

**必须先回滚再释放锁**，否则其他事务可能读到未回滚的脏数据。

### 1.7 关键数据结构

- **Transaction**: `txn_id_`, `state_`, `write_set_`, `lock_set_`
- **WriteRecord**: `wtype_`, `tab_name_`, `rid_`, `record_`
- **LockDataId**: `fd_`, `rid_`, `type_` (TABLE/RECORD) — 唯一标识一个锁目标
- **LockRequest**: `txn_id_`, `lock_mode_`, `granted_`
- **LockRequestQueue**: `request_queue_`, `group_lock_mode_`, `cv_`

---

## 2. 事务管理器 TransactionManager（3 个 TODO）

**文件**：`src/transaction/transaction_manager.cpp`

### TODO 2.1：begin — 开始事务

**位置**：第 23-31 行
**函数签名**：`Transaction * begin(Transaction* txn, LogManager* log_manager)`

```cpp
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // 如果传入的 txn 为空指针，创建全新事务
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
        txn->set_start_ts(next_timestamp_++);
        txn->set_state(TransactionState::GROWING);
    }
    // 加入全局事务表（加锁保护）
    {
        std::scoped_lock lock{latch_};
        txn_map[txn->get_transaction_id()] = txn;
    }
    return txn;
}
```

**为什么这么写**：`txn_map` 是全局共享数据结构，`commit`/`abort` 可能在其他线程中并发修改，必须加锁。

---

### TODO 2.2：commit — 提交事务

**位置**：第 38-46 行
**函数签名**：`void commit(Transaction* txn, LogManager* log_manager)`

```cpp
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // 1. 释放所有锁
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();
    // 2. 清理写集（释放内存）
    auto write_set = txn->get_write_set();
    for (auto &write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
    // 3. 刷日志
    log_manager->flush_log_to_disk();
    // 4. 更新状态
    txn->set_state(TransactionState::COMMITTED);
    // 5. 从全局表移除
    {
        std::scoped_lock lock{latch_};
        txn_map.erase(txn->get_transaction_id());
    }
}
```

**为什么先释放锁再清理写集**：`unlock()` 不依赖 `write_set_`，先释放锁可以让其他事务更快获取资源。

---

### TODO 2.3：abort — 回滚事务

**位置**：第 53-61 行
**函数签名**：`void abort(Transaction *txn, LogManager *log_manager)`

```cpp
void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    // 1. 逆序回滚所有写操作
    auto write_set = txn->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        WriteRecord *wr = *it;
        auto &fh = sm_manager_->get_rm_file_handle(wr->GetTableName());
        switch (wr->GetWriteType()) {
            case WType::INSERT_TUPLE:
                fh->delete_record(wr->GetRid(), nullptr);    // 插入的要删掉
                break;
            case WType::DELETE_TUPLE:
                fh->insert_record(wr->GetRecord().data, nullptr);  // 删除的要恢复
                break;
            case WType::UPDATE_TUPLE:
                fh->update_record(wr->GetRid(), wr->GetRecord().data, nullptr);  // 恢复旧值
                break;
        }
        delete wr;
    }
    write_set->clear();
    // 2. 释放所有锁（必须在回滚之后！）
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();
    // 3. 刷日志 + 更新状态
    log_manager->flush_log_to_disk();
    txn->set_state(TransactionState::ABORTED);
    // 4. 从全局表移除
    {
        std::scoped_lock lock{latch_};
        txn_map.erase(txn->get_transaction_id());
    }
}
```

**为什么必须先回滚再释放锁**：如果先释放锁，其他事务可能读到未回滚的脏数据，破坏原子性。

---

## 3. 锁管理器 LockManager（7 个 TODO）

**文件**：`src/transaction/concurrency/lock_manager.cpp`

### 前置知识：两个辅助函数

在实现 7 个 TODO 之前，添加两个辅助函数：

```cpp
// 检查锁兼容性
static bool is_lock_compatible(GroupLockMode granted_mode, LockMode request_mode) {
    switch (request_mode) {
        case LockMode::INTENTION_SHARED:
            return granted_mode == GroupLockMode::NON_LOCK ||
                   granted_mode == GroupLockMode::IS ||
                   granted_mode == GroupLockMode::IX ||
                   granted_mode == GroupLockMode::S;
        case LockMode::INTENTION_EXCLUSIVE:
            return granted_mode == GroupLockMode::NON_LOCK ||
                   granted_mode == GroupLockMode::IS ||
                   granted_mode == GroupLockMode::IX;
        case LockMode::SHARED:
            return granted_mode == GroupLockMode::NON_LOCK ||
                   granted_mode == GroupLockMode::IS ||
                   granted_mode == GroupLockMode::S;
        case LockMode::EXLUCSIVE:  // 注意：框架代码拼写错误，保持原样
            return granted_mode == GroupLockMode::NON_LOCK;
        case LockMode::S_IX:
            return granted_mode == GroupLockMode::NON_LOCK ||
                   granted_mode == GroupLockMode::IS;
        default:
            return false;
    }
}

// LockMode → GroupLockMode 映射
static GroupLockMode get_group_lock_mode(LockMode mode) {
    switch (mode) {
        case LockMode::INTENTION_SHARED:    return GroupLockMode::IS;
        case LockMode::INTENTION_EXCLUSIVE: return GroupLockMode::IX;
        case LockMode::SHARED:              return GroupLockMode::S;
        case LockMode::EXLUCSIVE:           return GroupLockMode::X;
        case LockMode::S_IX:                return GroupLockMode::SIX;
        default:                            return GroupLockMode::NON_LOCK;
    }
}
```

### 所有 7 个 TODO 的通用模式

```
1. std::scoped_lock lock{latch_}    — 保护 lock_table_
2. 构造 LockDataId                   — 唯一标识锁目标
3. 在 lock_table_ 中查找/创建请求队列
4. 检查该事务是否已持有此锁（避免重复授予）
5. 检查兼容性（is_lock_compatible）
   ├─ 兼容 → 授予锁，更新队列和事务的锁集
   └─ 冲突 → throw TransactionAbortException（No-Wait 策略）
```

### TODO 3.1：lock_shared_on_record（行级 S 锁）

**位置**：第 20 行 | **调用场景**：读取某行数据时

```cpp
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_data_id];
    // 检查是否已持有
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    // 检查兼容性
    if (!is_lock_compatible(queue.group_lock_mode_, LockMode::SHARED))
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    // 授予锁
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED);
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::S;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}
```

### TODO 3.2：lock_exclusive_on_record（行级 X 锁）

**位置**：第 32 行 | **调用场景**：修改/删除某行数据时

```cpp
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    if (!is_lock_compatible(queue.group_lock_mode_, LockMode::EXLUCSIVE))
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXLUCSIVE);
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::X;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}
```

### TODO 3.3：lock_shared_on_table（表级 S 锁）

**位置**：第 43 行

```cpp
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    if (!is_lock_compatible(queue.group_lock_mode_, LockMode::SHARED))
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::SHARED);
    queue.request_queue_.back().granted_ = true;
    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK || queue.group_lock_mode_ == GroupLockMode::IS)
        queue.group_lock_mode_ = GroupLockMode::S;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}
```

### TODO 3.4：lock_exclusive_on_table（表级 X 锁）

**位置**：第 54 行

```cpp
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    if (!is_lock_compatible(queue.group_lock_mode_, LockMode::EXLUCSIVE))
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXLUCSIVE);
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::X;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}
```

### TODO 3.5：lock_IS_on_table（表级 IS 锁）

**位置**：第 65 行

```cpp
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    if (!is_lock_compatible(queue.group_lock_mode_, LockMode::INTENTION_SHARED))
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_SHARED);
    queue.request_queue_.back().granted_ = true;
    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK)
        queue.group_lock_mode_ = GroupLockMode::IS;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}
```

### TODO 3.6：lock_IX_on_table（表级 IX 锁）

**位置**：第 76 行

```cpp
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    if (!is_lock_compatible(queue.group_lock_mode_, LockMode::INTENTION_EXCLUSIVE))
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE);
    queue.request_queue_.back().granted_ = true;
    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK || queue.group_lock_mode_ == GroupLockMode::IS)
        queue.group_lock_mode_ = GroupLockMode::IX;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}
```

### TODO 3.7：unlock（释放锁）

**位置**：第 87 行

```cpp
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    std::scoped_lock lock{latch_};
    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) return false;
    auto &queue = it->second;
    // 在队列中找到该事务的请求并移除
    for (auto req_it = queue.request_queue_.begin(); req_it != queue.request_queue_.end(); ++req_it) {
        if (req_it->txn_id_ == txn->get_transaction_id()) {
            queue.request_queue_.erase(req_it);
            break;
        }
    }
    // 重新计算组锁模式
    queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    for (auto &req : queue.request_queue_) {
        if (req.granted_) {
            GroupLockMode mode = get_group_lock_mode(req.lock_mode_);
            if (static_cast<int>(mode) > static_cast<int>(queue.group_lock_mode_))
                queue.group_lock_mode_ = mode;
        }
    }
    // 从事务的锁集中移除
    txn->get_lock_set()->erase(lock_data_id);
    return true;
}
```

**为什么重算 `group_lock_mode_`**：移除请求后，队列中剩余锁的最强模式可能变化。例如两个 S 锁移除一个后，仍然是 S；两个都移除后，变为 NON_LOCK。

---

## 4. 两阶段封锁协议集成（4 处修改）

### 4.1 rmdb.cpp — 启用事务 ID 设置（第 121 行）

```cpp
// 修改前：
// SetTransaction(&txn_id, context);
// 修改后：
SetTransaction(&txn_id, context);
```

**为什么需要**：没有这行，`context->txn_` 始终为空，所有锁操作都会因空指针崩溃。

### 4.2 rmdb.cpp — 启用自动提交（第 183-186 行）

```cpp
// 修改前：
// if(context->txn_->get_txn_mode() == false)
// {
//     txn_manager->commit(context->txn_, context->log_mgr_);
// }
// 修改后：
if(context->txn_->get_txn_mode() == false)
{
    txn_manager->commit(context->txn_, context->log_mgr_);
}
```

**为什么需要**：非显式事务的锁永远不会释放，后续所有冲突的锁请求都会被 abort。

### 4.3 rm_file_handle.cpp — get_record 加 S 锁

在 `get_record` 函数开头添加：

```cpp
context->lock_mgr_->lock_shared_on_record(context->txn_, rid, fd_);
```

### 4.4 rm_file_handle.cpp — delete_record 和 update_record 加 X 锁

在 `delete_record` 和 `update_record` 函数开头分别添加：

```cpp
context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
```

**完整的锁流程示例**：

```
T1: SELECT * FROM student WHERE id = 1;
  → SetTransaction 创建事务 T1 (GROWING)
  → get_record → lock_shared_on_record(T1, rid) → 授予 S 锁
  → 读取数据
  → 自动提交 → commit → 释放 S 锁

T2: UPDATE student SET name='Alice' WHERE id=1; (T1 未提交时)
  → SetTransaction 创建事务 T2
  → update_record → lock_exclusive_on_record(T2, rid)
  → 检查兼容性：队列有 T1 的 S 锁，X 与 S 不兼容
  → throw TransactionAbortException → T2 被 abort
```

---

## 5. 完整测试流程

### 第 0 步：启用事务集成代码

取消 `rmdb.cpp` 中的两处注释（见第 4 节）。

### 第 1 步：编译项目

```bash
cd build
make rmdb -j$(nproc)
```

### 第 2 步：事务管理器测试（40 分）

```bash
cd src/test/transaction
python transaction_test.py
```

| 测试点 | 内容 | 分数 |
|--------|------|------|
| `commit_test` | TPC-C 新订单事务，commit 后验证数据持久化 | 20 |
| `abort_test` | 同样的事务，abort 后验证数据已回滚 | 20 |

单独调试：

```bash
python transaction_unit_test.py commit_test
python transaction_unit_test.py abort_test
```

### 第 3 步：并发控制测试（60 分）

```bash
cd src/test/concurrency
python concurrency_test.py
```

| 测试点 | 内容 | 分数 |
|--------|------|------|
| `concurrency_read_test` | 两个事务并发读取不同行 | 10 |
| `dirty_write_test` | T1 写入后中止，T2 读取 — T2 看到原始值 | 10 |
| `dirty_read_test` | T1 写入未提交，T2 读取 — T2 不看到脏值 | 10 |
| `lost_update_test` | T1 读+更新，T2 更新同一行 — 不丢失更新 | 10 |
| `unrepeatable_read_test` | T1 读两次，T2 写入并中止 — T1 两次读相同 | 10 |
| `unrepeatable_read_test_hard` | T1 读两次，T2 写入并提交 — T1 两次读相同 | 10 |

单独调试：

```bash
python concurrency_unit_test.py dirty_write_test
python concurrency_unit_test.py dirty_read_test
```

---

## 6. 调试技巧

### 常见调试场景

**rmdb 启动崩溃** → 检查 `rmdb.cpp` 第 121 行 `SetTransaction` 是否取消注释

**abort 后数据未恢复** → 检查 INSERT/DELETE/UPDATE executor 中是否调用了 `txn->append_write_record(new WriteRecord(...))`

**死锁导致测试卡住** → 检查 `commit()`/`abort()` 是否遍历了 `lock_set_` 并逐一调用 `unlock`

### 调试命令

```bash
# 运行单个并发测试
python concurrency_unit_test.py dirty_write_test

# 查看服务器日志
cat output.txt

# GDB 调试
gdb ./build/rmdb
(gdb) break lock_manager.cpp:20
(gdb) run
```

---

## 7. 常见错误与解决方案

| 症状 | 根因 | 修复方法 |
|------|------|----------|
| rmdb 启动崩溃 | `SetTransaction` 未取消注释 | 取消 rmdb.cpp 第 121 行注释 |
| 单条 SQL 后 select 看不到结果 | auto-commit 未启用 | 取消 rmdb.cpp 第 183-186 行注释 |
| dirty_read_test 失败 | `get_record` 未加 S 锁 | 在 get_record 开头调用 lock_shared_on_record |
| dirty_write_test 失败 | `update_record` 未加 X 锁 | 在 update_record 开头调用 lock_exclusive_on_record |
| abort_test 失败 | write_set_ 未正确记录 | 检查 executor 中的 append_write_record 调用 |
| 测试卡住 | unlock 未正确释放 | 检查 commit/abort 中的锁释放循环 |
| 不可重复读测试失败 | S 锁在读取后立即释放 | S 锁必须持有到事务结束（commit/abort 时才释放） |
| 丢失更新测试失败 | 读取时未加锁 | 读取也必须加 S 锁 |

---

## 附录：各函数速查表

### 事务管理器

| 函数 | 文件:行 | 核心操作 |
|------|---------|----------|
| `begin` | `transaction_manager.cpp:23` | 创建事务，加入 txn_map |
| `commit` | `transaction_manager.cpp:38` | 释放所有锁，清空资源，设 COMMITTED |
| `abort` | `transaction_manager.cpp:53` | 逆序回滚写操作，释放锁，设 ABORTED |

### 锁管理器

| 函数 | 文件:行 | 核心操作 |
|------|---------|----------|
| `lock_shared_on_record` | `lock_manager.cpp:20` | 行级 S 锁，no-wait |
| `lock_exclusive_on_record` | `lock_manager.cpp:32` | 行级 X 锁，no-wait |
| `lock_shared_on_table` | `lock_manager.cpp:43` | 表级 S 锁 |
| `lock_exclusive_on_table` | `lock_manager.cpp:54` | 表级 X 锁 |
| `lock_IS_on_table` | `lock_manager.cpp:65` | 表级 IS 锁 |
| `lock_IX_on_table` | `lock_manager.cpp:76` | 表级 IX 锁 |
| `unlock` | `lock_manager.cpp:87` | 释放指定锁 |

### 集成点

| 修改点 | 文件:行 | 核心操作 |
|--------|---------|----------|
| SetTransaction | `rmdb.cpp:121` | 取消注释，启用事务 |
| auto-commit | `rmdb.cpp:183` | 取消注释，启用自动提交 |
| get_record + S 锁 | `rm_file_handle.cpp:19` | 读取前加共享锁 |
| delete_record + X 锁 | `rm_file_handle.cpp:77` | 删除前加排他锁 |
| update_record + X 锁 | `rm_file_handle.cpp:99` | 更新前加排他锁 |
