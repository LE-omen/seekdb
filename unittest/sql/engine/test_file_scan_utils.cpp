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
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "lib/container/ob_array.h"
#include "common/object/ob_object.h"
#include <stdio.h>
#include <string.h>

using namespace oceanbase::common;
using namespace oceanbase::sql;

class TestFileScanUtils : public ::testing::Test
{
protected:
  void SetUp() override {}
  void TearDown() override {}

  static void write_temp_file(const char *path, const char *content, size_t len)
  {
    FILE *fp = fopen(path, "wb");
    if (fp != NULL) {
      fwrite(content, 1, len, fp);
      fclose(fp);
    }
  }
};

TEST_F(TestFileScanUtils, CsvParseBasicAndQuotes)
{
  ObArenaAllocator allocator;
  FileScanOptions options;
  ObSEArray<FileColumnMeta, 16> columns;
  ObSEArray<ObArray<ObObj>, 64> rows;

  const char *tmp_path = "/tmp/test_csv_quotes.csv";
  const char *csv_data = "id,name,val\n"
                         "1,Alice,10.5\n"
                         "2,\"Bob, Jr.\",20.0\n"
                         "3,\"Multi\nline\",30.2\n"
                         "4,\"Escaped \"\"quote\"\"\",40.0\n";
  write_temp_file(tmp_path, csv_data, strlen(csv_data));

  int ret = ObFileScanUtils::scan_csv_file(allocator, ObString::make_string(tmp_path), options, columns, rows);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(3, columns.count());
  ASSERT_EQ(ObString::make_string("id"), columns.at(0).col_name_);
  ASSERT_EQ(ObString::make_string("name"), columns.at(1).col_name_);
  ASSERT_EQ(ObString::make_string("val"), columns.at(2).col_name_);

  ASSERT_EQ(4, rows.count());
  // Row 0
  ASSERT_EQ(1, rows.at(0).at(0).get_int());
  ASSERT_EQ(ObString::make_string("Alice"), rows.at(0).at(1).get_string());
  // Row 1: Bob, Jr.
  ASSERT_EQ(ObString::make_string("Bob, Jr."), rows.at(1).at(1).get_string());
  // Row 2: Multiline
  ASSERT_EQ(ObString::make_string("Multi\nline"), rows.at(2).at(1).get_string());
  // Row 3: Escaped quote
  ASSERT_EQ(ObString::make_string("Escaped \"quote\""), rows.at(3).at(1).get_string());
  remove(tmp_path);
}

TEST_F(TestFileScanUtils, CsvDuplicateHeaderDeduplication)
{
  ObArenaAllocator allocator;
  FileScanOptions options;
  ObSEArray<FileColumnMeta, 16> columns;
  ObSEArray<ObArray<ObObj>, 64> rows;

  const char *tmp_path = "/tmp/test_csv_dedup.csv";
  const char *csv_data = "id,name,id,id\n"
                         "1,A,10,100\n";
  write_temp_file(tmp_path, csv_data, strlen(csv_data));

  int ret = ObFileScanUtils::scan_csv_file(allocator, ObString::make_string(tmp_path), options, columns, rows);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(4, columns.count());
  ASSERT_EQ(ObString::make_string("id"), columns.at(0).col_name_);
  ASSERT_EQ(ObString::make_string("name"), columns.at(1).col_name_);
  ASSERT_EQ(ObString::make_string("id__2"), columns.at(2).col_name_);
  ASSERT_EQ(ObString::make_string("id__3"), columns.at(3).col_name_);
  remove(tmp_path);
}

TEST_F(TestFileScanUtils, CsvTypeInference)
{
  ObArenaAllocator allocator;
  FileScanOptions options;
  ObSEArray<FileColumnMeta, 16> columns;
  ObSEArray<ObArray<ObObj>, 64> rows;

  const char *tmp_path = "/tmp/test_csv_types.csv";
  const char *csv_data = "c_int,c_double,c_bool,c_date,c_datetime,c_str\n"
                         "123,45.67,true,2026-10-27,2026-10-27 23:00:00,hello\n"
                         "456,89.01,false,2026-10-28,2026-10-28 12:00:00,world\n";
  write_temp_file(tmp_path, csv_data, strlen(csv_data));

  int ret = ObFileScanUtils::scan_csv_file(allocator, ObString::make_string(tmp_path), options, columns, rows);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(6, columns.count());
  ASSERT_EQ(ObIntType, columns.at(0).col_type_);
  ASSERT_EQ(ObDoubleType, columns.at(1).col_type_);
  ASSERT_EQ(ObTinyIntType, columns.at(2).col_type_); // boolean
  ASSERT_EQ(ObDateType, columns.at(3).col_type_);
  ASSERT_EQ(ObDateTimeType, columns.at(4).col_type_);
  ASSERT_EQ(ObVarcharType, columns.at(5).col_type_);
  remove(tmp_path);
}

TEST_F(TestFileScanUtils, JsonlUnionSchemaAndMissingFields)
{
  ObArenaAllocator allocator;
  FileScanOptions options;
  options.format_ = ObString::make_string("newline_delimited");
  ObSEArray<FileColumnMeta, 16> columns;
  ObSEArray<ObArray<ObObj>, 64> rows;

  const char *tmp_path = "/tmp/test_jsonl.jsonl";
  const char *jsonl_data = "{\"id\": 1, \"name\": \"Alice\"}\n"
                           "{\"id\": 2, \"score\": 95.5}\n"
                           "{\"id\": 3, \"name\": \"Charlie\", \"score\": 88.0}\n";
  write_temp_file(tmp_path, jsonl_data, strlen(jsonl_data));

  int ret = ObFileScanUtils::scan_json_file(allocator, ObString::make_string(tmp_path), options, columns, rows);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(3, columns.count());
  ASSERT_EQ(3, rows.count());

  // Row 0 has score = NULL
  ASSERT_TRUE(rows.at(0).at(2).is_null());
  // Row 1 has name = NULL
  ASSERT_TRUE(rows.at(1).at(1).is_null());
  // Row 2 has all 3 non-null
  ASSERT_FALSE(rows.at(2).at(0).is_null());
  ASSERT_FALSE(rows.at(2).at(1).is_null());
  ASSERT_FALSE(rows.at(2).at(2).is_null());
  remove(tmp_path);
}

TEST_F(TestFileScanUtils, JsonlRejectNestedObjects)
{
  ObArenaAllocator allocator;
  FileScanOptions options;
  ObSEArray<FileColumnMeta, 16> columns;
  ObSEArray<ObArray<ObObj>, 64> rows;

  const char *tmp_path = "/tmp/test_nested.jsonl";
  const char *jsonl_data = "{\"id\": 1, \"meta\": {\"nested\": true}}\n";
  write_temp_file(tmp_path, jsonl_data, strlen(jsonl_data));

  int ret = ObFileScanUtils::scan_json_file(allocator, ObString::make_string(tmp_path), options, columns, rows);
  ASSERT_EQ(OB_NOT_SUPPORTED, ret);
  remove(tmp_path);
}

TEST_F(TestFileScanUtils, SnappyDecompress)
{
  // Literal Snappy frame: 5 bytes uncompressed length 5, then "hello"
  const char compressed[] = "\x05\x10hello";
  char uncompressed[16];
  int64_t uncompressed_len = sizeof(uncompressed);

  int ret = ObFileScanUtils::snappy_decompress(compressed, sizeof(compressed) - 1, uncompressed, uncompressed_len);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(5, uncompressed_len);
  ASSERT_EQ(0, memcmp(uncompressed, "hello", 5));
}
