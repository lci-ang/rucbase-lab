/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

    bool evaluate_condition(const Condition &cond, RmRecord *record) {
        auto lhs_col = get_col(cols_, cond.lhs_col);
        char *lhs_data = record->data + lhs_col->offset;
        char *rhs_data = nullptr;
        if (cond.is_rhs_val) {
            rhs_data = cond.rhs_val.raw->data;
        } else {
            auto rhs_col = get_col(cols_, cond.rhs_col);
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

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

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

    std::unique_ptr<RmRecord> Next() override {
        auto record = std::make_unique<RmRecord>(len_);
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();
        memcpy(record->data, left_rec->data, left_->tupleLen());
        memcpy(record->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return record;
    }

    bool is_end() const override {
        return isend;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return _abstract_rid; }
};