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

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta cols_;                              // 框架中只支持一个键排序，需要自行修改数据结构支持多个键排序
    size_t tuple_num;
    bool is_desc_;
    std::vector<size_t> used_tuple;
    std::unique_ptr<RmRecord> current_tuple;
    std::vector<std::unique_ptr<RmRecord>> sorted_records_;
    size_t current_idx_ = 0;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        cols_ = prev_->get_col_offset(sel_cols);
        is_desc_ = is_desc;
        tuple_num = 0;
        used_tuple.clear();
    }

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

    void nextTuple() override {
        current_idx_++;
        if (current_idx_ < tuple_num) {
            current_tuple = std::move(sorted_records_[used_tuple[current_idx_]]);
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(current_tuple);
    }

    bool is_end() const override {
        return current_idx_ >= tuple_num;
    }

    Rid &rid() override { return _abstract_rid; }
};