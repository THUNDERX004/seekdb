/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _OB_LOG_JSON_TABLE_H
#define _OB_LOG_JSON_TABLE_H
#include "sql/optimizer/ob_logical_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
class ObLogJsonTable : public ObLogicalOperator
{
public:
  ObLogJsonTable(ObLogPlan &plan)
      : ObLogicalOperator(plan),
        table_id_(OB_INVALID_ID),
        value_exprs_(),
        table_name_(),
        access_exprs_(),
        all_cols_def_(),
        table_type_(MulModeTableType::INVALID_TABLE_TYPE),
        namespace_arr_(),
        file_path_(),
        file_options_(),
        file_stat_(),
        file_row_count_(0) {}

  virtual ~ObLogJsonTable() {}
  int add_values_expr(ObIArray<ObRawExpr*> &exprs) { return append(value_exprs_, exprs); }
  const ObIArray<ObRawExpr*> &get_value_expr() const { return value_exprs_; }
  ObIArray<ObRawExpr*> &get_value_expr() { return value_exprs_; }
  virtual uint64_t hash(uint64_t seed) const override;
  int generate_access_exprs();
  ObIArray<ObRawExpr*> &get_access_exprs() { return access_exprs_; }
  int get_op_exprs(ObIArray<ObRawExpr*> &all_exprs);
  virtual int allocate_expr_post(ObAllocExprContext &ctx) override;
  void set_table_id(uint64_t table_id) { table_id_ = table_id; }
  uint64_t get_table_id() const { return table_id_; }

  inline common::ObString &get_table_name() { return table_name_; }
  inline const common::ObString &get_table_name() const { return table_name_; }
  inline void set_table_name(const common::ObString &table_name) { table_name_ = table_name; }

  common::ObIArray<ObJtColBaseInfo*>& get_origin_cols_def() { return all_cols_def_; }
  virtual int get_plan_item_info(PlanText &plan_text, 
                                ObSqlPlanItem &plan_item) override;
  inline void set_table_type(MulModeTableType table_type) { table_type_ = table_type; }
  inline MulModeTableType get_table_type() { return table_type_; }
  int set_namespace_arr(ObIArray<ObString> &namespace_arr);
  int get_namespace_arr(ObIArray<ObString> &namespace_arr);
  int64_t get_ns_size() { return namespace_arr_.count(); }
  int64_t get_column_param_default_size() { return column_param_default_exprs_.count(); }
  int set_column_param_default_arr(ObIArray<ObColumnDefault> &column_param_default_exprs);
  ObColumnDefault* get_column_param_default_val(int64_t index);
  void set_file_table_info(const common::ObString &path,
                           const file_sql::FileOptions &options,
                           const file_sql::FileStat &stat,
                           const int64_t row_count)
  {
    file_path_ = path;
    file_options_ = options;
    file_stat_ = stat;
    file_row_count_ = row_count;
  }
  const common::ObString &get_file_path() const { return file_path_; }
  const file_sql::FileOptions &get_file_options() const { return file_options_; }
  const file_sql::FileStat &get_file_stat() const { return file_stat_; }
  int64_t get_file_row_count() const { return file_row_count_; }
  bool is_file_table_path() const
  {
    return MulModeTableType::OB_FILE_CSV_TABLE_TYPE == table_type_
        || MulModeTableType::OB_FILE_JSON_TABLE_TYPE == table_type_
        || MulModeTableType::OB_FILE_PARQUET_TABLE_TYPE == table_type_
        || MulModeTableType::OB_FILE_GLOB_TABLE_TYPE == table_type_
        || MulModeTableType::OB_FILE_CSV_DESCRIBE_TABLE_TYPE == table_type_
        || MulModeTableType::OB_FILE_JSON_DESCRIBE_TABLE_TYPE == table_type_
        || MulModeTableType::OB_FILE_PARQUET_DESCRIBE_TABLE_TYPE == table_type_;
  }
private:
  uint64_t table_id_;
  common::ObSEArray<ObRawExpr*, 1, common::ModulePageAllocator, true> value_exprs_;
  common::ObString table_name_;
  common::ObSEArray<ObRawExpr*, 4, common::ModulePageAllocator, true> access_exprs_;

  common::ObSEArray<ObJtColBaseInfo*, 4, common::ModulePageAllocator, true> all_cols_def_;
  // table func type
  MulModeTableType table_type_;
  // xml table param
  common::ObSEArray<ObString, 16, common::ModulePageAllocator, true> namespace_arr_;
  // default value array
  common::ObSEArray<ObColumnDefault, 1, common::ModulePageAllocator, true> column_param_default_exprs_;
  common::ObString file_path_;
  file_sql::FileOptions file_options_;
  file_sql::FileStat file_stat_;
  int64_t file_row_count_;

  DISALLOW_COPY_AND_ASSIGN(ObLogJsonTable);
};

}
}
#endif
