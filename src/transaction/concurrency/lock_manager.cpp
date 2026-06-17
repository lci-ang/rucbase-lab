/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) return true;
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

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) return true;
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            // 锁升级：检查是否有其他事务持有锁，有则abort
            for (auto &other : queue.request_queue_) {
                if (other.txn_id_ != txn->get_transaction_id() && other.granted_) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
            }
            // 如果已有S锁，升级为X锁
            if (req.lock_mode_ == LockMode::SHARED) {
                req.lock_mode_ = LockMode::EXLUCSIVE;
                queue.group_lock_mode_ = GroupLockMode::X;
            }
            return true;
        }
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

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    // 检查兼容性：S 锁兼容 NON_LOCK / IS / IX / S
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

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            // 锁升级：检查是否有其他事务持有锁，有则abort
            for (auto &other : queue.request_queue_) {
                if (other.txn_id_ != txn->get_transaction_id() && other.granted_) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
            }
            // S/IS/IX -> X
            if (req.lock_mode_ != LockMode::EXLUCSIVE) {
                req.lock_mode_ = LockMode::EXLUCSIVE;
                queue.group_lock_mode_ = GroupLockMode::X;
            }
            return true;
        }
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

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    // 检查兼容性：IS 锁兼容 NON_LOCK / IS / IX / S
    if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK &&
        queue.group_lock_mode_ != GroupLockMode::IS &&
        queue.group_lock_mode_ != GroupLockMode::IX &&
        queue.group_lock_mode_ != GroupLockMode::S)
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_SHARED);
    queue.request_queue_.back().granted_ = true;
    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK)
        queue.group_lock_mode_ = GroupLockMode::IS;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    if (txn == nullptr) return true;
    std::scoped_lock lock{latch_};
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }
    // 检查兼容性：IX 锁兼容 NON_LOCK / IS / IX
    if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK &&
        queue.group_lock_mode_ != GroupLockMode::IS &&
        queue.group_lock_mode_ != GroupLockMode::IX)
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE);
    queue.request_queue_.back().granted_ = true;
    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK || queue.group_lock_mode_ == GroupLockMode::IS)
        queue.group_lock_mode_ = GroupLockMode::IX;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
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
    // 重新计算组锁模式
    queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    for (auto &req : queue.request_queue_) {
        if (req.granted_) {
            // 取最强的锁模式
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
    // 从事务的锁集中移除
    txn->get_lock_set()->erase(lock_data_id);
    return true;
}
