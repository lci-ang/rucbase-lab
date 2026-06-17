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
    // Bug fix: 先复制lock_set再遍历 — unlock内部会erase，直接遍历导致迭代器失效(segfault)
    auto lock_ids = *txn->get_lock_set();
    for (auto &lock_data_id : lock_ids) {
        lock_manager_->unlock(txn, lock_data_id);
    }
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
    // 5. 从全局事务表中移除
    // 注意：不删除txn_map条目！否则下次SetTransaction调get_transaction会断言失败
    // 保留在map中，SetTransaction通过状态检查(COMMITTED)自动创建新事务
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
        // 注意：SmManager 没有 get_rm_file_handle() 方法，用 fhs_ map 直接访问
        auto *fh = sm_manager_->fhs_.at(wr->GetTableName()).get();
        switch (wr->GetWriteType()) {
            case WType::INSERT_TUPLE:
                fh->delete_record(wr->GetRid(), nullptr);    // 插入的要删掉
                break;
            case WType::DELETE_TUPLE:
                // Bug fix: 必须恢复到原rid，否则后续INSERT_TUPLE回滚的delete_record会删错位置
                fh->insert_record(wr->GetRid(), wr->GetRecord().data);
                break;
            case WType::UPDATE_TUPLE:
                // 注意：推荐在UpdateExecutor中用DELETE_TUPLE+INSERT_TUPLE替代UPDATE_TUPLE
                // 因为delete+insert可能换rid，单独update_record无法处理这种情况
                fh->delete_record(wr->GetRid(), nullptr);  // 先删除新记录
                fh->insert_record(wr->GetRid(), wr->GetRecord().data);  // 再恢复旧记录
                break;
        }
        delete wr;
    }
    write_set->clear();
    // 注意：不删除txn_map条目！理由同commit
    // 2. 释放所有锁...
    // Bug fix: 先复制lock_set再遍历 — unlock内部会erase
    auto lock_ids = *txn->get_lock_set();
    for (auto &lock_data_id : lock_ids) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();
    // 3. 刷日志 + 更新状态
    log_manager->flush_log_to_disk();
    txn->set_state(TransactionState::ABORTED);
    // 4. 从全局事务表中移除
    // 注意：不删除txn_map条目！否则下次SetTransaction调get_transaction会断言失败
    // 理由：get_transaction中对不存在的txn_id有assert，但commit/abort后txn_id未重置
    // 保留在map中，SetTransaction通过状态检查(ABORTED)自动创建新事务
}
```

**为什么必须先回滚再释放锁**：如果先释放锁，其他事务可能读到未回滚的脏数据，破坏原子性。

---

## 3. 锁管理器 LockManager（7 个 TODO）

**文件**：`src/transaction/concurrency/lock_manager.cpp`

### 关键约束：LockMode / GroupLockMode 是 private enum

`lock_manager.h` 中 `LockMode` 和 `GroupLockMode` 定义在 `LockManager` 类的 **private** 区域。这意味着不能在 `.cpp` 文件中声明独立的 `static` 辅助函数（它们无法访问 private 类型）。**必须把兼容性检查直接内联到每个锁函数中**。

### 所有 7 个 TODO 的通用模式

```
1. if (txn == nullptr) return true;    — 事务未初始化时跳过（rmdb.cpp未启用时防崩溃）
2. std::scoped_lock lock{latch_}       — 保护 lock_table_
3. 构造 LockDataId                     — 唯一标识锁目标
4. 在 lock_table_ 中查找/创建请求队列
5. 检查该事务是否已持有此锁（避免重复授予）
6. 检查兼容性（内联兼容矩阵判断）
   ├─ 兼容 → 授予锁，更新队列和事务的 lock_set_
   └─ 冲突 → throw TransactionAbortException（No-Wait 策略）
```

### TODO 3.1：lock_shared_on_record（行级 S 锁）

**位置**：第 58 行 | **调用场景**：读取某行数据时

```cpp
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) return true;  // 事务未初始化时跳过
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_data_id];
    // 检查是否已持有此锁
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    // 检查兼容性：S 锁兼容 NON_LOCK / IS / IX / S
    if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK &&
        queue.group_lock_mode_ != GroupLockMode::IS &&
        queue.group_lock_mode_ != GroupLockMode::IX &&
        queue.group_lock_mode_ != GroupLockMode::S)
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

**位置**：第 84 行 | **调用场景**：修改/删除某行数据时

```cpp
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) return true;
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    // 检查兼容性：X 锁只兼容 NON_LOCK
    if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK)
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXLUCSIVE);
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::X;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}
```

### TODO 3.3：lock_shared_on_table（表级 S 锁）

```cpp
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    // S 锁兼容 NON_LOCK / IS / IX / S
    if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK &&
        queue.group_lock_mode_ != GroupLockMode::IS &&
        queue.group_lock_mode_ != GroupLockMode::IX &&
        queue.group_lock_mode_ != GroupLockMode::S)
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

```cpp
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    // 其余同模式，只是 LockDataType::TABLE 和兼容性检查不同
    // X 锁只兼容 NON_LOCK
    ...
}
```

### TODO 3.5：lock_IS_on_table（表级 IS 锁）

```cpp
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    // IS 锁兼容 NON_LOCK / IS / IX / S
    // 授予后若队列为空 → group_lock_mode_ = IS
    ...
}
```

### TODO 3.6：lock_IX_on_table（表级 IX 锁）

```cpp
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    // IX 锁兼容 NON_LOCK / IS / IX
    // 授予后若队列为 NON_LOCK 或 IS → group_lock_mode_ = IX
    ...
}
```

### TODO 3.7：unlock（释放锁）

**位置**：第 200 行

```cpp
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) return true;
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
    // 重新计算组锁模式：遍历剩余请求，取最强锁模式
    queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    for (auto &req : queue.request_queue_) {
        if (req.granted_) {
            switch (req.lock_mode_) {
                case LockMode::EXLUCSIVE:
                    queue.group_lock_mode_ = GroupLockMode::X; break;
                case LockMode::S_IX:
                    if (queue.group_lock_mode_ != GroupLockMode::X) queue.group_lock_mode_ = GroupLockMode::SIX; break;
                case LockMode::SHARED:
                    if (queue.group_lock_mode_ != GroupLockMode::X && queue.group_lock_mode_ != GroupLockMode::SIX)
                        queue.group_lock_mode_ = GroupLockMode::S; break;
                case LockMode::INTENTION_EXCLUSIVE:
                    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK || queue.group_lock_mode_ == GroupLockMode::IS)
                        queue.group_lock_mode_ = GroupLockMode::IX; break;
                case LockMode::INTENTION_SHARED:
                    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK)
                        queue.group_lock_mode_ = GroupLockMode::IS; break;
                default: break;
            }
        }
    }
    txn->get_lock_set()->erase(lock_data_id);
    return true;
}
```

**为什么重算 `group_lock_mode_`**：移除请求后，队列中剩余锁的最强模式可能变化。不能再用 `get_group_lock_mode` 辅助函数（private enum 不可访问），改用 switch 内联判断。

**为什么每个函数都加 `if (txn == nullptr) return true`**：`rmdb.cpp` 中 `SetTransaction` 被注释时 `context->txn_` 为 nullptr。不加保护会导致空指针崩溃（segfault）。加保护后，无事务时跳过锁逻辑，不影响普通 SQL 执行。

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

在 `get_record` 函数开头（TODO 注释后）添加：

```cpp
// 加行级S锁（context为空或txn未初始化时跳过）
if (context != nullptr && context->txn_ != nullptr)
    context->lock_mgr_->lock_shared_on_record(context->txn_, rid, fd_);
```

### 4.4 rm_file_handle.cpp — delete_record 和 update_record 加 X 锁

在 `delete_record` 和 `update_record` 函数开头分别添加：

```cpp
// 加行级X锁（context为空或txn未初始化时跳过）
if (context != nullptr && context->txn_ != nullptr)
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

### 4.5 executor 中维护 write_set（3 个文件）

**重要**：事务回滚依赖 `write_set_`，但 Lab3 的执行器不记录写操作。必须在三个执行器中添加 `append_write_record` 调用。

#### InsertExecutor (`executor_insert.h`)

在 `fh_->insert_record()` 之后添加：

```cpp
// 记录写操作到write_set，用于回滚
if (context_->txn_ != nullptr) {
    context_->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_));
}
```

#### DeleteExecutor (`executor_delete.h`)

在 `fh_->get_record()` 之后、删除之前添加：

```cpp
// 记录写操作到write_set，用于回滚（必须在删除前保存旧值）
if (context_->txn_ != nullptr) {
    context_->txn_->append_write_record(new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *record));
}
```

#### UpdateExecutor (`executor_update.h`)

**关键发现**：UpdateExecutor 中 `delete_record(old_rid)` + `insert_record(new_rid)` 可能导致 rid 变化。如果只记录一个 `UPDATE_TUPLE` WriteRecord（含 old_rid），回滚时无法找到 new_rid 上的新记录。

**正确做法**：记录两个 WriteRecord — `DELETE_TUPLE`(旧记录) + `INSERT_TUPLE`(新记录)：

```cpp
// 删除旧记录，插入新记录
fh_->delete_record(rid, context_);
Rid new_rid = fh_->insert_record(new_record.data, context_);
// 记录写操作到write_set — 注意顺序！
// 回滚时逆序处理：先delete_record(new_rid)，再insert_record(rid, old_data)
if (context_->txn_ != nullptr) {
    context_->txn_->append_write_record(new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *old_record));
    context_->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name_, new_rid));
}
```

### 4.6 锁升级（Lock Upgrade）

**关键发现**：当同一事务先获取 S 锁再获取 X 锁时，直接 `return true` 跳过兼容性检查会导致 `group_lock_mode_` 不更新（仍为 S），其他事务仍能获取 S 锁，导致写冲突检测失效。

在每个 X 锁函数中，处理"已有锁"时需要检查并升级：

```cpp
for (auto &req : queue.request_queue_) {
    if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
        // 锁升级：如果已有S锁，升级为X锁
        if (req.lock_mode_ == LockMode::SHARED) {
            req.lock_mode_ = LockMode::EXLUCSIVE;
            queue.group_lock_mode_ = GroupLockMode::X;
        }
        return true;
    }
}
```

同样适用于 `lock_exclusive_on_table`（IS/IX/S → X）。

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
| dirty_write_test / lost_update_test / unrepeatable_read_test 失败 | 锁升级(S→X)未检查其他事务的锁，导致两事务同时持有X锁 | 升级前遍历队列：若有其他事务的 granted 锁则 throw DEADLOCK_PREVENTION |
| concurrency_read_test 通过但写测试全失败 | `insert_record` 不加锁，新记录无保护 | insert_record 返回前对新 rid 加 X 锁 |
| abort_test 失败 | write_set_ 未正确记录 | 检查 executor 中的 append_write_record 调用 |
| 测试卡住 | unlock 未正确释放 | 检查 commit/abort 中的锁释放循环 |
| 不可重复读测试失败 | S 锁在读取后立即释放 | S 锁必须持有到事务结束（commit/abort 时才释放） |
| 丢失更新测试失败 | 读取时未加锁 | 读取也必须加 S 锁 |
| lock_manager.cpp 编译报 `GroupLockMode was not declared` | LockMode/GroupLockMode 是 private enum | 不能写独立的 static 辅助函数，须把兼容性判断内联到每个锁函数中 |
| `'class SmManager' has no member named 'get_rm_file_handle'` | SmManager 无此方法 | 改用 `sm_manager_->fhs_.at(tab_name).get()` |
| rmdb 段错误 (nullptr dereference) | txn 未初始化就调用了 lock 函数 | 每个 lock 函数开头加 `if (txn == nullptr) return true`，lock 调用处也加 context 非空判断 |
| commit/abort 后在 "select..." 时服务器崩溃 (assertion failed) | `get_transaction` 对已删除的 txn_id 触发 assert；根源是 commit/abort 中 `txn_map.erase()` 删除了事务但 `txn_id` 未重置 | **不在 commit/abort 中调用 `txn_map.erase()`**。保留已提交/已回滚的事务在 map 中，`SetTransaction` 会通过状态检查(`COMMITTED`/`ABORTED`)自动创建新事务 |
| commit 时 segfault (核心已转储) | `commit()` 遍历 `lock_set` 时 `unlock()` 内部调用 `lock_set->erase()`，导致迭代器失效 | 先复制 lock_set 再遍历：`auto lock_ids = *txn->get_lock_set()` |
| abort_test 失败，数据未回滚 | `write_set_` 从未被填充 — 执行器未调用 `append_write_record()` | 在 InsertExecutor、DeleteExecutor、UpdateExecutor 中添加 WriteRecord 记录 |
| abort 后数据出现重复行（同一记录出现两次） | `DELETE_TUPLE` 回滚使用 `insert_record(buf)` 分配了新 rid，旧 INSERT WriteRecord 的 `delete_record(old_rid)` 删错位置 | 用 `insert_record(wr->GetRid(), wr->GetRecord().data)` 恢复到原 rid |
| abort 后 UPDATE 的数据仍显示新值 | UpdateExecutor 中 `delete_record`+`insert_record` 可能产生不同 rid，单个 UPDATE_TUPLE WriteRecord 无法追踪新 rid | 在 UpdateExecutor 中用 `DELETE_TUPLE`(旧记录) + `INSERT_TUPLE`(新记录) 两个 WriteRecord 替代一个 UPDATE_TUPLE |
| 脏写/脏读测试失败，写冲突未检测到 | 事务 S→X 锁升级时，`lock_exclusive_on_record` 仅 `return true` 未更新 `group_lock_mode_`，其他事务检测到的组锁模式仍为 S | 在 X 锁函数中检测已有 S 锁时，同时更新 `req.lock_mode_ = LockMode::EXLUCSIVE` 和 `queue.group_lock_mode_ = GroupLockMode::X` |

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
