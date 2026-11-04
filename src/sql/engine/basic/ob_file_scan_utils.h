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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

enum class FileFormat : int8_t
{
  INVALID = 0,
  CSV = 1,
  JSON = 2,
  PARQUET = 3,
  GLOB = 4,
  CSV_DESCRIBE = 5,
  JSON_DESCRIBE = 6,
  PARQUET_DESCRIBE = 7,
};

enum class ColumnType : int8_t
{
  INVALID = 0,
  BIGINT,
  DOUBLE,
  BOOLEAN,
  VARCHAR,
  DATE,
  DATETIME,
};

struct FileOptions
{
  FileOptions()
    : header_(true),
      newline_delimited_(true),
      sample_size_(-1),
      delimiter_(','),
      quote_('"'),
      escape_('\\')
  {}

  bool header_;
  bool newline_delimited_;
  int64_t sample_size_;
  char delimiter_;
  char quote_;
  char escape_;
  std::string null_string_;
};

struct FileStat
{
  FileStat() : size_(0), modify_time_(0) {}
  int64_t size_;
  int64_t modify_time_;
};

struct ColumnSchema
{
  ColumnSchema() : type_(ColumnType::INVALID), nullable_(true) {}

  std::string name_;
  ColumnType type_;
  bool nullable_;
};

struct TableSchema
{
  TableSchema() : row_count_(0) {}

  std::vector<ColumnSchema> columns_;
  FileStat stat_;
  int64_t row_count_;
};

struct ScalarValue
{
  ScalarValue() : is_null_(true) {}

  bool is_null_;
  std::string text_;
};

class IFileReader
{
public:
  virtual ~IFileReader() {}
  virtual int rescan() = 0;
  virtual int get_next_row(std::vector<ScalarValue> &row) = 0;
};

int get_file_stat(const std::string &path, FileStat &stat);
int infer_table_schema(const std::string &path,
                       const FileFormat format,
                       const FileOptions &options,
                       TableSchema &schema);
int create_file_reader(const std::string &path,
                       const FileFormat format,
                       const FileOptions &options,
                       const TableSchema &schema,
                       const std::vector<int64_t> &projection,
                       std::unique_ptr<IFileReader> &reader);
int list_glob_files(const std::string &pattern, std::vector<std::string> &files);

} // namespace file_sql
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
