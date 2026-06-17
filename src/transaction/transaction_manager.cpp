/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
        txn->set_start_ts(next_timestamp_++);
        txn->set_state(TransactionState::GROWING);
    }
    // 3. 把开始事务加入到全局事务表中
    {
        std::scoped_lock lock{latch_};
        txn_map[txn->get_transaction_id()] = txn;
    }
    // 4. 返回当前事务指针
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // 1. 释放所有锁
    // Bug fix: 先复制lock_set再遍历，因为unlock内部会erase导致迭代器失效
    auto lock_ids = *txn->get_lock_set();
    for (auto &lock_data_id : lock_ids) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    // 2. 释放事务相关资源，eg.写集
    auto write_set = txn->get_write_set();
    for (auto &write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
    // 3. 把事务日志刷入磁盘中
    log_manager->flush_log_to_disk();
    // 4. 更新事务状态
    txn->set_state(TransactionState::COMMITTED);
    // 5. 从全局事务表中移除
    // 注意：不删除txn_map中的条目，否则SetTransaction中get_transaction会断言失败
    // 保留在map中，SetTransaction会通过状态检查(COMMITTED)自动创建新事务
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    // 1. 逆序回滚所有写操作（必须先回滚再释放锁）
    auto write_set = txn->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        WriteRecord *wr = *it;
        auto *fh = sm_manager_->fhs_.at(wr->GetTableName()).get();
        switch (wr->GetWriteType()) {
            case WType::INSERT_TUPLE:
                fh->delete_record(wr->GetRid(), nullptr);
                break;
            case WType::DELETE_TUPLE:
                fh->insert_record(wr->GetRid(), wr->GetRecord().data);  // 必须恢复到原rid，否则后续INSERT_TUPLE回滚会删错位置
                break;
            case WType::UPDATE_TUPLE:
                fh->delete_record(wr->GetRid(), nullptr);  // 先删除旧位置上的新记录
                fh->insert_record(wr->GetRid(), wr->GetRecord().data);  // 在原位置恢复旧记录
                break;
        }
        delete wr;
    }
    write_set->clear();
    // 2. 释放所有锁（必须在回滚之后）
    // Bug fix: 先复制lock_set再遍历，因为unlock内部会erase导致迭代器失效
    auto lock_ids = *txn->get_lock_set();
    for (auto &lock_data_id : lock_ids) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    txn->get_lock_set()->clear();
    // 3. 把事务日志刷入磁盘中
    log_manager->flush_log_to_disk();
    // 4. 更新事务状态
    txn->set_state(TransactionState::ABORTED);
    // 5. 从全局事务表中移除
    // 注意：不删除txn_map中的条目，否则SetTransaction中get_transaction会断言失败
    // 保留在map中，SetTransaction会通过状态检查(ABORTED)自动创建新事务
}