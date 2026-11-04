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

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "share/ob_errno.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

namespace
{

class TempDirectory
{
public:
  TempDirectory()
  {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path()
        / ("seekdb-file-sql-" + std::to_string(now));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory()
  {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  std::string file(const std::string &name) const
  {
    return (path_ / name).string();
  }
  void write(const std::string &name, const std::string &content) const
  {
    std::ofstream out(path_ / name, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    ASSERT_TRUE(out.good());
  }

private:
  std::filesystem::path path_;
};

std::string decode_base64(const std::string &text)
{
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  uint32_t value = 0;
  int bits = -8;
  for (const char c : text) {
    const char *pos = strchr(alphabet, c);
    if (nullptr == pos) {
      continue;
    }
    value = (value << 6) | static_cast<uint32_t>(pos - alphabet);
    bits += 6;
    if (bits >= 0) {
      output.push_back(static_cast<char>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  return output;
}

} // namespace

TEST(TestFileScanUtils, csv_schema_and_streaming_reader)
{
  TempDirectory temp;
  temp.write("input.csv",
             "id,name,id,d,empty\n"
             "1,\"Alice\",x,2024-01-01,\"\"\n"
             "2,\"line\nbreak\",y,2024-02-03 04:05:06,\"\"\n");
  FileOptions options;
  TableSchema schema;
  ASSERT_EQ(OB_SUCCESS, infer_table_schema(temp.file("input.csv"),
                                            FileFormat::CSV,
                                            options,
                                            schema));
  ASSERT_EQ(5, schema.columns_.size());
  EXPECT_EQ("id__2", schema.columns_[2].name_);
  EXPECT_EQ(ColumnType::BIGINT, schema.columns_[0].type_);
  EXPECT_EQ(ColumnType::VARCHAR, schema.columns_[1].type_);
  EXPECT_EQ(ColumnType::DATETIME, schema.columns_[3].type_);
  EXPECT_EQ(ColumnType::VARCHAR, schema.columns_[4].type_);
  EXPECT_FALSE(schema.columns_[4].nullable_);

  std::unique_ptr<IFileReader> reader;
  ASSERT_EQ(OB_SUCCESS, create_file_reader(temp.file("input.csv"),
                                            FileFormat::CSV,
                                            options,
                                            schema,
                                            {0, 1, 2, 3, 4},
                                            reader));
  std::vector<ScalarValue> row;
  ASSERT_EQ(OB_SUCCESS, reader->get_next_row(row));
  ASSERT_EQ(5, row.size());
  EXPECT_EQ("1", row[0].text_);
  EXPECT_EQ("Alice", row[1].text_);
  ASSERT_EQ(OB_SUCCESS, reader->get_next_row(row));
  EXPECT_EQ("line\nbreak", row[1].text_);
  EXPECT_EQ(OB_ITER_END, reader->get_next_row(row));
  ASSERT_EQ(OB_SUCCESS, reader->rescan());
  ASSERT_EQ(OB_SUCCESS, reader->get_next_row(row));
  EXPECT_EQ("Alice", row[1].text_);
}

TEST(TestFileScanUtils, json_union_missing_and_nested_rejection)
{
  TempDirectory temp;
  temp.write("input.jsonl",
             "{\"id\":1,\"score\":1}\n"
             "{\"id\":2,\"score\":2.5,\"extra\":\"x\"}\n"
             "{\"id\":null,\"score\":null,\"extra\":null}\n");
  FileOptions options;
  TableSchema schema;
  ASSERT_EQ(OB_SUCCESS, infer_table_schema(temp.file("input.jsonl"),
                                            FileFormat::JSON,
                                            options,
                                            schema));
  ASSERT_EQ(3, schema.columns_.size());
  EXPECT_EQ(ColumnType::BIGINT, schema.columns_[0].type_);
  EXPECT_EQ(ColumnType::DOUBLE, schema.columns_[1].type_);
  EXPECT_EQ(ColumnType::VARCHAR, schema.columns_[2].type_);
  EXPECT_TRUE(schema.columns_[2].nullable_);

  std::unique_ptr<IFileReader> reader;
  ASSERT_EQ(OB_SUCCESS, create_file_reader(temp.file("input.jsonl"),
                                            FileFormat::JSON,
                                            options,
                                            schema,
                                            {0, 1, 2},
                                            reader));
  std::vector<ScalarValue> row;
  ASSERT_EQ(OB_SUCCESS, reader->get_next_row(row));
  EXPECT_TRUE(row[2].is_null_);
  ASSERT_EQ(OB_SUCCESS, reader->get_next_row(row));
  EXPECT_EQ("x", row[2].text_);

  temp.write("nested.jsonl", "{\"id\":1,\"nested\":{\"x\":2}}\n");
  EXPECT_EQ(OB_NOT_SUPPORTED,
            infer_table_schema(temp.file("nested.jsonl"),
                               FileFormat::JSON,
                               options,
                               schema));
}

TEST(TestFileScanUtils, csv_custom_dialect_and_null_text)
{
  TempDirectory temp;
  temp.write("custom.csv", "id;name\n1;NULL\n2;|hello;world|\n");
  FileOptions options;
  options.delimiter_ = ';';
  options.quote_ = '|';
  options.escape_ = '|';
  options.null_string_ = "NULL";
  TableSchema schema;
  ASSERT_EQ(OB_SUCCESS, infer_table_schema(temp.file("custom.csv"),
                                            FileFormat::CSV,
                                            options,
                                            schema));
  ASSERT_EQ(2, schema.columns_.size());
  EXPECT_EQ(ColumnType::BIGINT, schema.columns_[0].type_);
  EXPECT_EQ(ColumnType::VARCHAR, schema.columns_[1].type_);
  EXPECT_TRUE(schema.columns_[1].nullable_);

  std::unique_ptr<IFileReader> reader;
  ASSERT_EQ(OB_SUCCESS, create_file_reader(temp.file("custom.csv"),
                                            FileFormat::CSV,
                                            options,
                                            schema,
                                            {0, 1},
                                            reader));
  std::vector<ScalarValue> row;
  ASSERT_EQ(OB_SUCCESS, reader->get_next_row(row));
  EXPECT_TRUE(row[1].is_null_);
  ASSERT_EQ(OB_SUCCESS, reader->get_next_row(row));
  EXPECT_EQ("hello;world", row[1].text_);
}

TEST(TestFileScanUtils, glob_and_file_change_detection)
{
  TempDirectory temp;
  temp.write("a.csv", "id\n1\n");
  temp.write("b.txt", "text\n");
  std::vector<std::string> files;
  ASSERT_EQ(OB_SUCCESS, list_glob_files(
      (std::filesystem::path(temp.file("a.csv")).parent_path() / "*.csv").string(),
      files));
  ASSERT_EQ(1, files.size());
  EXPECT_NE(std::string::npos, files[0].find("a.csv"));

  FileStat before;
  FileStat after;
  ASSERT_EQ(OB_SUCCESS, get_file_stat(temp.file("a.csv"), before));
  temp.write("a.csv", "id\n1\n2\n");
  ASSERT_EQ(OB_SUCCESS, get_file_stat(temp.file("a.csv"), after));
  EXPECT_NE(before.size_, after.size_);
}

TEST(TestFileScanUtils, snappy_parquet_footer_and_reader)
{
  static const char parquet_base64[] =
      "UEFSMRUGFTwVQFwVDhUAFQ4VABUEFQARHDYAKAQGAAAAGAQAAAAAAAAAA38cbAAAAAAB"
      "AAAAAgAAAAMAAAAEAAAABQAAAAYAAAAmlAEcFQIZJQYAGRgCYzEVAhYOFogBFowBJgg8"
      "NgAoBAYAAAAYBAAAAAAAAAAVBBksSARyb290FQIAFQIlAhgCYzEAFg4ZHBkcJpQBHBUC"
      "GSUGABkYAmMxFQIWDhaIARaMASYIPDYAKAQGAAAAGAQAAAAAAAAAFowBFg4AGRwYDEFS"
      "Uk9XOnNjaGVtYRi8AS8vLy8vNFFBQUFBUUFBQUFBQUFLQUE0QURBQUxBQVFBQ2dBQUFC"
      "UUFBQUFBQUFBQkJBQUtBQXdBQUFBSUFBUUFDZ0FBQUFnQUFBQUlBQUFBQUFBQUFBRUFB"
      "QUFVQUFBQUVBQVVBQkFBRGdBUEFBUUFBQUFJQUJBQUFBQVlBQUFBSUFBQUFBQUFBUUlj"
      "QUFBQUNBQU1BQVFBQ3dBSUFBQUFJQUFBQUFBQUFBRUFBQUFBQWdBQUFHTXhBQUE9ABgs"
      "QXJyb3cyIC0gTmF0aXZlIFJ1c3QgaW1wbGVtZW50YXRpb24gb2YgQXJyb3cATgEAAFBB"
      "UjE=";
  TempDirectory temp;
  std::ofstream out(temp.file("simple.parquet"), std::ios::binary);
  const std::string bytes = decode_base64(parquet_base64);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();

  FileOptions options;
  TableSchema schema;
  ASSERT_EQ(OB_SUCCESS, infer_table_schema(temp.file("simple.parquet"),
                                            FileFormat::PARQUET,
                                            options,
                                            schema));
  ASSERT_EQ(1, schema.columns_.size());
  EXPECT_EQ(ColumnType::BIGINT, schema.columns_[0].type_);
  EXPECT_EQ(7, schema.row_count_);

  std::unique_ptr<IFileReader> reader;
  ASSERT_EQ(OB_SUCCESS, create_file_reader(temp.file("simple.parquet"),
                                            FileFormat::PARQUET,
                                            options,
                                            schema,
                                            {0},
                                            reader));
  int64_t sum = 0;
  int64_t rows = 0;
  std::vector<ScalarValue> row;
  int ret = OB_SUCCESS;
  while (OB_SUCCESS == (ret = reader->get_next_row(row))) {
    sum += atoll(row[0].text_.c_str());
    ++rows;
  }
  EXPECT_EQ(OB_ITER_END, ret);
  EXPECT_EQ(7, rows);
  EXPECT_EQ(21, sum);
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
