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

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "lib/allocator/ob_arena_allocator.h"
#include "lib/charset/ob_charset.h"
#include "share/ob_errno.h"
#include "sql/parser/ob_sql_parser.h"

namespace oceanbase
{
namespace sql
{

namespace
{

void collect_file_tables(ParseNode *node, std::vector<ParseNode *> &nodes)
{
  if (nullptr != node) {
    if (T_FILE_SQL_EXPRESSION == node->type_) {
      nodes.push_back(node);
    }
    for (int64_t i = 0; i < node->num_child_; ++i) {
      collect_file_tables(node->children_[i], nodes);
    }
  }
}

void parse_file_tables(ObArenaAllocator &allocator,
                       const char *sql,
                       std::vector<ParseNode *> &nodes)
{
  ObSQLParser parser(allocator, SMO_DEFAULT);
  ParseResult result;
  MEMSET(&result, 0, sizeof(result));
  result.malloc_pool_ = &allocator;
  result.sql_mode_ = SMO_DEFAULT;
  result.charset_info_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
  result.charset_info_nls_db_ = result.charset_info_;
  result.connection_collation_ = CS_TYPE_UTF8MB4_GENERAL_CI;
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  ASSERT_EQ(OB_SUCCESS, parser.parse(sql, static_cast<int64_t>(strlen(sql)), result));
  ASSERT_NE(nullptr, result.result_tree_);
  collect_file_tables(result.result_tree_, nodes);
  EXPECT_EQ(OB_SUCCESS, parse_terminate(&result));
}

} // namespace

TEST(TestFileSqlParser, read_functions_and_alias)
{
  ObArenaAllocator allocator("FileSqlParserTest");
  std::vector<ParseNode *> nodes;
  parse_file_tables(
      allocator,
      "SELECT * FROM read_csv('/tmp/a.csv', header = true, sample_size = -1) AS f",
      nodes);
  ASSERT_EQ(1, nodes.size());
  ASSERT_EQ(4, nodes[0]->num_child_);
  ASSERT_NE(nullptr, nodes[0]->children_[0]);
  EXPECT_EQ(0, strncmp("read_csv", nodes[0]->children_[0]->str_value_, 8));
  ASSERT_NE(nullptr, nodes[0]->children_[3]);
  EXPECT_EQ(1, nodes[0]->children_[3]->str_len_);
  EXPECT_EQ('f', nodes[0]->children_[3]->str_value_[0]);
}

TEST(TestFileSqlParser, json_parquet_and_glob)
{
  {
    ObArenaAllocator allocator("FileSqlParserTest");
    std::vector<ParseNode *> nodes;
    parse_file_tables(
        allocator,
        "SELECT * FROM read_json('/tmp/a.jsonl', format = 'newline_delimited') j",
        nodes);
    ASSERT_EQ(1, nodes.size());
  }
  {
    ObArenaAllocator allocator("FileSqlParserTest");
    std::vector<ParseNode *> nodes;
    parse_file_tables(
        allocator,
        "SELECT * FROM read_ndjson('/tmp/a.jsonl')",
        nodes);
    ASSERT_EQ(1, nodes.size());
  }
  {
    ObArenaAllocator allocator("FileSqlParserTest");
    std::vector<ParseNode *> nodes;
    parse_file_tables(
        allocator,
        "SELECT * FROM read_parquet('/tmp/a.parquet') p",
        nodes);
    ASSERT_EQ(1, nodes.size());
  }
  {
    ObArenaAllocator allocator("FileSqlParserTest");
    std::vector<ParseNode *> nodes;
    parse_file_tables(allocator, "SELECT * FROM glob('/tmp/*.csv') g", nodes);
    ASSERT_EQ(1, nodes.size());
  }
}

TEST(TestFileSqlParser, describe_rewrites_to_metadata_function)
{
  ObArenaAllocator allocator("FileSqlParserTest");
  std::vector<ParseNode *> nodes;
  parse_file_tables(
      allocator,
      "DESCRIBE SELECT * FROM read_csv('/tmp/a.csv', header = false)",
      nodes);
  ASSERT_EQ(1, nodes.size());
  ASSERT_NE(nullptr, nodes[0]->children_[0]);
  EXPECT_EQ(0, strncmp("read_csv_describe",
                       nodes[0]->children_[0]->str_value_,
                       17));
}

TEST(TestFileSqlParser, describe_does_not_rewrite_identifier)
{
  ObArenaAllocator allocator("FileSqlParserTest");
  std::vector<ParseNode *> nodes;
  parse_file_tables(
      allocator,
      "DESCRIBE SELECT read_csv AS c FROM t",
      nodes);
  EXPECT_TRUE(nodes.empty());
}

TEST(TestFileSqlParser, copy_statement_is_rewritten)
{
  ObArenaAllocator allocator("FileSqlParserTest");
  std::vector<ParseNode *> nodes;
  parse_file_tables(
      allocator,
      "COPY (SELECT c1 FROM t ORDER BY c1) TO '/tmp/out.csv' "
      "(FORMAT CSV, HEADER, DELIMITER ';', QUOTE '\"', "
      "ESCAPE '\\'', NULLSTR 'NULL')",
      nodes);
  EXPECT_TRUE(nodes.empty());
}

TEST(TestFileSqlParser, join_accepts_two_file_tables)
{
  ObArenaAllocator allocator("FileSqlParserTest");
  std::vector<ParseNode *> nodes;
  parse_file_tables(
      allocator,
      "SELECT c.id, j.name "
      "FROM read_csv('/tmp/c.csv') c "
      "LEFT JOIN read_json('/tmp/j.jsonl') j ON c.id = j.id",
      nodes);
  ASSERT_EQ(2, nodes.size());
}

} // namespace sql
} // namespace oceanbase
