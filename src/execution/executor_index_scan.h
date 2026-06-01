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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

    bool evaluate_condition(const Condition &cond, RmRecord *record) {
        auto lhs_col = tab_.get_col(cond.lhs_col.col_name);
        char *lhs_data = record->data + lhs_col->offset;
        char *rhs_data = nullptr;
        if (cond.is_rhs_val) {
            rhs_data = cond.rhs_val.raw->data;
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

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        auto ih = sm_manager_->ihs_.at(
            sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)
        ).get();

        Iid lower = ih->leaf_begin();
        Iid upper = ih->leaf_end();

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
                    // cond.rhs_val.raw 已在 analyze 阶段初始化，直接使用
                    lower = ih->lower_bound(cond.rhs_val.raw->data);
                    upper = ih->upper_bound(cond.rhs_val.raw->data);
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

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override {
        return scan_->is_end();
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return rid_; }
};