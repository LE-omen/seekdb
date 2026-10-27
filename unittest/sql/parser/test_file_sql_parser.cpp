/*
 * Copyright (c) 2026 OceanBase.
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

#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

class TestFileSqlParser : public ::testing::Test
{
protected:
  void SetUp() override {}
  void TearDown() override {}

  int parse(const char *sql_str, ParseResult &result, ObArenaAllocator &allocator)
  {
    ObParser parser(allocator, 0); // MySQL mode
    ObString stmt = ObString::make_string(sql_str);
    return parser.parse_sql(stmt, result, false);
  }
};

TEST_F(TestFileSqlParser, ParseReadCsvBasic)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "SELECT * FROM read_csv('/tmp/test.csv');";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ASSERT_EQ(T_STMT_LIST, result.result_tree_->type_);
  ASSERT_GT(result.result_tree_->num_child_, 0);

  ParseNode *select_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(select_stmt != NULL);
  ASSERT_EQ(T_SELECT_STMT, select_stmt->type_);
}

TEST_F(TestFileSqlParser, ParseReadCsvWithOptions)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "SELECT * FROM read_csv('/tmp/test.csv', header=true, delim=',', sample_size=100);";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ASSERT_EQ(T_STMT_LIST, result.result_tree_->type_);

  ParseNode *select_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(select_stmt != NULL);
  ASSERT_EQ(T_SELECT_STMT, select_stmt->type_);
}

TEST_F(TestFileSqlParser, ParseReadJsonAndNdjson)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql1 = "SELECT * FROM read_json('/tmp/data.json');";
  int ret1 = parse(sql1, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret1);

  const char *sql2 = "SELECT * FROM read_ndjson('/tmp/data.ndjson');";
  int ret2 = parse(sql2, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret2);
}

TEST_F(TestFileSqlParser, ParseReadParquet)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "SELECT id, name FROM read_parquet('/tmp/data.parquet');";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
}

TEST_F(TestFileSqlParser, ParseGlob)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "SELECT file FROM glob('/tmp/*.csv');";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
}

TEST_F(TestFileSqlParser, ParseDescribeSelect)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql1 = "DESCRIBE SELECT * FROM read_csv('/tmp/test.csv');";
  int ret1 = parse(sql1, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret1);

  const char *sql2 = "DESC SELECT id FROM t1;";
  int ret2 = parse(sql2, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret2);
}

TEST_F(TestFileSqlParser, ParseCopyTo)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql1 = "COPY (SELECT * FROM t1) TO '/tmp/out.csv' WITH (format='csv', header=true, delimiter=',');";
  int ret1 = parse(sql1, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret1);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ParseNode *copy_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(copy_stmt != NULL);
  ASSERT_EQ(T_COPY_TO_FILE, copy_stmt->type_);

  const char *sql2 = "COPY t1 TO '/tmp/out.csv';";
  int ret2 = parse(sql2, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret2);
  ParseNode *copy_stmt2 = result.result_tree_->children_[0];
  ASSERT_TRUE(copy_stmt2 != NULL);
  ASSERT_EQ(T_COPY_TO_FILE, copy_stmt2->type_);

  const char *sql3 = "COPY (select id, name from read_csv('/tmp/test.csv') order by id) TO '/tmp/export.csv' (format csv, header, nullstr 'NULL');";
  int ret3 = parse(sql3, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret3);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ParseNode *copy_stmt3 = result.result_tree_->children_[0];
  ASSERT_TRUE(copy_stmt3 != NULL);
  ASSERT_EQ(T_COPY_TO_FILE, copy_stmt3->type_);
}

TEST_F(TestFileSqlParser, ParseNegativeSampleSize)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "SELECT count(*) FROM read_csv('/tmp/test.csv', header = true, sample_size = -1);";
  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
}

