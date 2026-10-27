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
#include "sql/engine/basic/ob_file_scan_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

class TestFileScanUtils : public ::testing::Test
{
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(TestFileScanUtils, CsvParseBasicAndQuotes)
{
  ObArenaAllocator allocator;
  ObFileScanResult result(allocator);

  const char *csv_data = "id,name,val\n"
                         "1,Alice,10.5\n"
                         "2,\"Bob, Jr.\",20.0\n"
                         "3,\"Multi\nline\",30.2\n"
                         "4,\"Escaped \"\"quote\"\"\",40.0\n";

  int ret = ObFileScanUtils::parse_csv_content(csv_data, strlen(csv_data), true, ',', result);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(3, result.column_names_.count());
  ASSERT_EQ("id", result.column_names_.at(0));
  ASSERT_EQ("name", result.column_names_.at(1));
  ASSERT_EQ("val", result.column_names_.at(2));

  ASSERT_EQ(4, result.rows_.count());
  // Row 0
  ASSERT_EQ("1", result.rows_.at(0).at(0).str_);
  ASSERT_EQ("Alice", result.rows_.at(0).at(1).str_);
  // Row 1: Bob, Jr.
  ASSERT_EQ("Bob, Jr.", result.rows_.at(1).at(1).str_);
  // Row 2: Multiline
  ASSERT_EQ("Multi\nline", result.rows_.at(2).at(1).str_);
  // Row 3: Escaped quote
  ASSERT_EQ("Escaped \"quote\"", result.rows_.at(3).at(1).str_);
}

TEST_F(TestFileScanUtils, CsvDuplicateHeaderDeduplication)
{
  ObArenaAllocator allocator;
  ObFileScanResult result(allocator);

  const char *csv_data = "id,name,id,id\n"
                         "1,A,10,100\n";

  int ret = ObFileScanUtils::parse_csv_content(csv_data, strlen(csv_data), true, ',', result);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(4, result.column_names_.count());
  ASSERT_EQ("id", result.column_names_.at(0));
  ASSERT_EQ("name", result.column_names_.at(1));
  ASSERT_EQ("id__2", result.column_names_.at(2));
  ASSERT_EQ("id__3", result.column_names_.at(3));
}

TEST_F(TestFileScanUtils, CsvTypeInference)
{
  ObArenaAllocator allocator;
  ObFileScanResult result(allocator);

  const char *csv_data = "c_int,c_double,c_bool,c_date,c_datetime,c_str\n"
                         "123,45.67,true,2026-10-27,2026-10-27 23:00:00,hello\n"
                         "456,89.01,false,2026-10-28,2026-10-28 12:00:00,world\n";

  int ret = ObFileScanUtils::parse_csv_content(csv_data, strlen(csv_data), true, ',', result);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(6, result.column_types_.count());
  ASSERT_EQ(ObIntType, result.column_types_.at(0).get_type());
  ASSERT_EQ(ObDoubleType, result.column_types_.at(1).get_type());
  ASSERT_EQ(ObTinyIntType, result.column_types_.at(2).get_type()); // boolean
  ASSERT_EQ(ObDateType, result.column_types_.at(3).get_type());
  ASSERT_EQ(ObDateTimeType, result.column_types_.at(4).get_type());
  ASSERT_EQ(ObVarcharType, result.column_types_.at(5).get_type());
}

TEST_F(TestFileScanUtils, JsonlUnionSchemaAndMissingFields)
{
  ObArenaAllocator allocator;
  ObFileScanResult result(allocator);

  const char *jsonl_data = "{\"id\": 1, \"name\": \"Alice\"}\n"
                           "{\"id\": 2, \"score\": 95.5}\n"
                           "{\"id\": 3, \"name\": \"Charlie\", \"score\": 88.0}\n";

  int ret = ObFileScanUtils::parse_jsonl_content(jsonl_data, strlen(jsonl_data), result);
  ASSERT_EQ(OB_SUCCESS, ret);
  // Column names union: id, name, score
  ASSERT_EQ(3, result.column_names_.count());
  ASSERT_EQ(3, result.rows_.count());

  // Row 0 has score = NULL
  ASSERT_TRUE(result.rows_.at(0).at(2).is_null_);
  // Row 1 has name = NULL
  ASSERT_TRUE(result.rows_.at(1).at(1).is_null_);
  // Row 2 has all 3 non-null
  ASSERT_FALSE(result.rows_.at(2).at(0).is_null_);
  ASSERT_FALSE(result.rows_.at(2).at(1).is_null_);
  ASSERT_FALSE(result.rows_.at(2).at(2).is_null_);
}

TEST_F(TestFileScanUtils, JsonlRejectNestedObjects)
{
  ObArenaAllocator allocator;
  ObFileScanResult result(allocator);

  const char *jsonl_data = "{\"id\": 1, \"meta\": {\"nested\": true}}\n";
  int ret = ObFileScanUtils::parse_jsonl_content(jsonl_data, strlen(jsonl_data), result);
  ASSERT_NE(OB_SUCCESS, ret);
}

TEST_F(TestFileScanUtils, SnappyDecompress)
{
  ObArenaAllocator allocator;
  // Literal Snappy frame: 5 bytes uncompressed length 5, then "hello"
  const char compressed[] = "\x05\x10hello";
  char uncompressed[16];
  int64_t uncompressed_len = sizeof(uncompressed);

  int ret = ObFileScanUtils::snappy_decompress(compressed, sizeof(compressed) - 1, uncompressed, uncompressed_len);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(5, uncompressed_len);
  ASSERT_EQ(0, memcmp(uncompressed, "hello", 5));
}
