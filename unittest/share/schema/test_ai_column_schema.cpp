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
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_table_schema.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

class TestAiColumnSchema : public ::testing::Test
{
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(TestAiColumnSchema, MetadataInitializationAndValidity)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema schema(&allocator);

  // Default values
  ASSERT_FALSE(schema.is_valid());
  ASSERT_EQ(OB_INVALID_ID, schema.get_table_id());
  ASSERT_EQ(OB_INVALID_ID, schema.get_column_id());
  ASSERT_EQ(2, schema.get_max_retries());
  ASSERT_TRUE(schema.get_column_name().empty());

  // Populate valid metadata
  schema.set_table_id(1001);
  schema.set_column_id(16);
  ASSERT_EQ(OB_SUCCESS, schema.set_column_name(ObString::make_string("tags")));
  ASSERT_EQ(OB_SUCCESS, schema.set_sync_mode(ObString::make_string("async")));
  schema.set_max_retries(3);
  schema.set_state_column_id(101);
  schema.set_request_column_id(102);
  schema.set_result_spec_column_id(103);
  ASSERT_EQ(OB_SUCCESS, schema.set_raw_expr(ObString::make_string("AI_COMPLETE('m', content)")));
  ASSERT_EQ(OB_SUCCESS, schema.set_normalized_expr(ObString::make_string("AI_COMPLETE('m', content) Return exactly one valid JSON value.")));

  ASSERT_TRUE(schema.is_valid());
  ASSERT_EQ(1001, schema.get_table_id());
  ASSERT_EQ(16, schema.get_column_id());
  ASSERT_EQ("tags", schema.get_column_name());
  ASSERT_EQ("async", schema.get_sync_mode());
  ASSERT_EQ(3, schema.get_max_retries());
  ASSERT_EQ(101, schema.get_state_column_id());
  ASSERT_EQ(102, schema.get_request_column_id());
  ASSERT_EQ(103, schema.get_result_spec_column_id());
  ASSERT_EQ("AI_COMPLETE('m', content)", schema.get_raw_expr());
  ASSERT_TRUE(schema.get_normalized_expr().find("Return exactly one valid JSON value.") != NULL);
}

TEST_F(TestAiColumnSchema, DeepCopyAndAssignment)
{
  ObArenaAllocator allocator1;
  ObArenaAllocator allocator2;
  ObAiColumnSchema src(&allocator1);

  src.set_table_id(2001);
  src.set_column_id(20);
  ASSERT_EQ(OB_SUCCESS, src.set_column_name(ObString::make_string("emb")));
  ASSERT_EQ(OB_SUCCESS, src.set_sync_mode(ObString::make_string("async")));
  src.set_max_retries(4);
  src.set_state_column_id(201);
  src.set_request_column_id(202);
  src.set_result_spec_column_id(203);
  ASSERT_EQ(OB_SUCCESS, src.set_raw_expr(ObString::make_string("AI_EMBED('em', content)")));
  ASSERT_EQ(OB_SUCCESS, src.set_normalized_expr(ObString::make_string("AI_EMBED('em', content)")));

  ObAiColumnSchema dst(&allocator2);
  ASSERT_EQ(OB_SUCCESS, dst.assign(src));

  // Verify deep copy values
  ASSERT_EQ(2001, dst.get_table_id());
  ASSERT_EQ(20, dst.get_column_id());
  ASSERT_EQ("emb", dst.get_column_name());
  ASSERT_EQ("async", dst.get_sync_mode());
  ASSERT_EQ(4, dst.get_max_retries());
  ASSERT_EQ(201, dst.get_state_column_id());
  ASSERT_EQ(202, dst.get_request_column_id());
  ASSERT_EQ(203, dst.get_result_spec_column_id());
  ASSERT_EQ("AI_EMBED('em', content)", dst.get_raw_expr());
  ASSERT_EQ("AI_EMBED('em', content)", dst.get_normalized_expr());

  // String buffers must be independent
  ASSERT_NE(src.get_column_name().ptr(), dst.get_column_name().ptr());
  ASSERT_NE(src.get_raw_expr().ptr(), dst.get_raw_expr().ptr());

  // Convert size calculation
  int64_t convert_size = dst.get_convert_size();
  ASSERT_GT(convert_size, sizeof(ObAiColumnSchema));
}

TEST_F(TestAiColumnSchema, SerializationAndDeserialization)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema original(&allocator);

  original.set_table_id(3001);
  original.set_column_id(33);
  ASSERT_EQ(OB_SUCCESS, original.set_column_name(ObString::make_string("summary")));
  ASSERT_EQ(OB_SUCCESS, original.set_sync_mode(ObString::make_string("async")));
  original.set_max_retries(2);
  original.set_state_column_id(301);
  original.set_request_column_id(302);
  original.set_result_spec_column_id(303);
  ASSERT_EQ(OB_SUCCESS, original.set_raw_expr(ObString::make_string("AI_COMPLETE('model1', text)")));
  ASSERT_EQ(OB_SUCCESS, original.set_normalized_expr(ObString::make_string("AI_COMPLETE('model1', text)")));

  int64_t serialize_size = original.get_serialize_size();
  ASSERT_GT(serialize_size, 0);

  char *buf = static_cast<char *>(allocator.alloc(serialize_size));
  ASSERT_TRUE(buf != NULL);

  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, original.serialize(buf, serialize_size, pos));
  ASSERT_EQ(serialize_size, pos);

  ObAiColumnSchema deserialized(&allocator);
  int64_t decode_pos = 0;
  ASSERT_EQ(OB_SUCCESS, deserialized.deserialize(buf, serialize_size, decode_pos));
  ASSERT_EQ(serialize_size, decode_pos);

  ASSERT_EQ(original.get_table_id(), deserialized.get_table_id());
  ASSERT_EQ(original.get_column_id(), deserialized.get_column_id());
  ASSERT_EQ(original.get_column_name(), deserialized.get_column_name());
  ASSERT_EQ(original.get_sync_mode(), deserialized.get_sync_mode());
  ASSERT_EQ(original.get_max_retries(), deserialized.get_max_retries());
  ASSERT_EQ(original.get_state_column_id(), deserialized.get_state_column_id());
  ASSERT_EQ(original.get_request_column_id(), deserialized.get_request_column_id());
  ASSERT_EQ(original.get_result_spec_column_id(), deserialized.get_result_spec_column_id());
  ASSERT_EQ(original.get_raw_expr(), deserialized.get_raw_expr());
  ASSERT_EQ(original.get_normalized_expr(), deserialized.get_normalized_expr());
}

TEST_F(TestAiColumnSchema, ColumnIdRemapping)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema schema(&allocator);

  schema.set_table_id(4001);
  schema.set_column_id(10);
  schema.set_state_column_id(100);
  schema.set_request_column_id(101);
  schema.set_result_spec_column_id(102);

  // Remap column_id from 10 to 20
  schema.remap_column_id(10, 20);
  ASSERT_EQ(20, schema.get_column_id());
  ASSERT_EQ(100, schema.get_state_column_id());

  // Remap state_column_id from 100 to 200
  schema.remap_column_id(100, 200);
  ASSERT_EQ(200, schema.get_state_column_id());

  // Remap request_column_id from 101 to 201
  schema.remap_column_id(101, 201);
  ASSERT_EQ(201, schema.get_request_column_id());

  // Remap result_spec_column_id from 102 to 202
  schema.remap_column_id(102, 202);
  ASSERT_EQ(202, schema.get_result_spec_column_id());

  // Remapping an unrelated ID must have no effect
  schema.remap_column_id(999, 888);
  ASSERT_EQ(20, schema.get_column_id());
  ASSERT_EQ(200, schema.get_state_column_id());
  ASSERT_EQ(201, schema.get_request_column_id());
  ASSERT_EQ(202, schema.get_result_spec_column_id());
}

TEST_F(TestAiColumnSchema, HiddenColumnRelationship)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema schema(&allocator);

  schema.set_column_id(15);
  schema.set_state_column_id(501);
  schema.set_request_column_id(502);
  schema.set_result_spec_column_id(503);

  // Hidden tracking columns must return true
  ASSERT_TRUE(schema.is_hidden_column(501));
  ASSERT_TRUE(schema.is_hidden_column(502));
  ASSERT_TRUE(schema.is_hidden_column(503));

  // User-facing AI column itself is not a hidden tracking column
  ASSERT_FALSE(schema.is_hidden_column(15));
  // Arbitrary column IDs
  ASSERT_FALSE(schema.is_hidden_column(1));
  ASSERT_FALSE(schema.is_hidden_column(500));
}

TEST_F(TestAiColumnSchema, TableSchemaIntegration)
{
  ObArenaAllocator allocator;
  ObTableSchema table(&allocator);
  table.set_table_id(5001);
  table.set_table_name("docs");

  ASSERT_FALSE(table.has_ai_column());

  ObAiColumnSchema col1(&allocator);
  col1.set_table_id(5001);
  col1.set_column_id(16);
  ASSERT_EQ(OB_SUCCESS, col1.set_column_name(ObString::make_string("tags")));
  col1.set_state_column_id(101);
  col1.set_request_column_id(102);
  col1.set_result_spec_column_id(103);
  ASSERT_EQ(OB_SUCCESS, table.add_ai_column(col1));

  ObAiColumnSchema col2(&allocator);
  col2.set_table_id(5001);
  col2.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS, col2.set_column_name(ObString::make_string("emb")));
  col2.set_state_column_id(201);
  col2.set_request_column_id(202);
  col2.set_result_spec_column_id(203);
  ASSERT_EQ(OB_SUCCESS, table.add_ai_column(col2));

  ASSERT_TRUE(table.has_ai_column());
  ASSERT_TRUE(table.is_ai_column(16));
  ASSERT_TRUE(table.is_ai_column(17));
  ASSERT_FALSE(table.is_ai_column(1));

  const ObAiColumnSchema *found = NULL;
  ASSERT_EQ(OB_SUCCESS, table.get_ai_column_schema(16, found));
  ASSERT_TRUE(found != NULL);
  ASSERT_EQ("tags", found->get_column_name());

  found = NULL;
  ASSERT_EQ(OB_SUCCESS, table.get_ai_column_schema(ObString::make_string("emb"), found));
  ASSERT_TRUE(found != NULL);
  ASSERT_EQ(17, found->get_column_id());

  // Copy table schema preserves AI columns
  ObTableSchema copied_table(&allocator);
  ASSERT_EQ(OB_SUCCESS, copied_table.assign(table));
  ASSERT_TRUE(copied_table.has_ai_column());
  ASSERT_TRUE(copied_table.is_ai_column(16));
  ASSERT_TRUE(copied_table.is_ai_column(17));
}
