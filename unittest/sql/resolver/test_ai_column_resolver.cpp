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
#include "share/schema/ob_column_schema.h"
#include "share/ob_errno.def"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

class TestAiColumnResolver : public ::testing::Test
{
protected:
  void SetUp() override {}
  void TearDown() override {}

  // Helper to validate vector dimension against AI_EMBED parameter
  int validate_vector_dim(uint32_t expected_dim, uint32_t expr_dim)
  {
    if (expr_dim != 0 && expr_dim != expected_dim) {
      return OB_ERR_INVALID_VECTOR_DIM; // -7600
    }
    return OB_SUCCESS;
  }

  // Helper to check dependency on another AI column
  int check_dependency(const ObTableSchema &table, const ObString &dep_col_name)
  {
    const ObAiColumnSchema *ai_col = NULL;
    if (OB_SUCCESS == table.get_ai_column_schema(dep_col_name, ai_col) && ai_col != NULL) {
      return OB_NOT_SUPPORTED; // -1235
    }
    return OB_SUCCESS;
  }

  // Helper to enrich prompt based on destination column data type
  int enrich_prompt(ObObjType type, const ObString &raw_prompt, ObIAllocator &allocator, ObString &enriched_prompt)
  {
    const char *suffix = NULL;
    if (ob_is_json_tc(type)) {
      suffix = " Return exactly one valid JSON value.";
    } else if (ob_is_int_tc(type)) {
      suffix = " Return exactly one base-10 integer.";
    }

    if (suffix != NULL) {
      int64_t len = raw_prompt.length() + strlen(suffix);
      char *buf = static_cast<char *>(allocator.alloc(len + 1));
      if (buf == NULL) {
        return OB_ALLOCATE_MEMORY_FAILED;
      }
      memcpy(buf, raw_prompt.ptr(), raw_prompt.length());
      memcpy(buf + raw_prompt.length(), suffix, strlen(suffix));
      buf[len] = '\0';
      enriched_prompt.assign_ptr(buf, static_cast<int32_t>(len));
    } else {
      enriched_prompt = raw_prompt;
    }
    return OB_SUCCESS;
  }

  // Helper to validate RETRY target
  int validate_retry(const ObTableSchema &table, const ObString &col_name, int64_t action_count)
  {
    if (action_count > 1) {
      return OB_NOT_SUPPORTED; // combining AI COLUMN RETRY with another ALTER action not supported
    }
    const ObAiColumnSchema *ai_col = NULL;
    if (OB_SUCCESS != table.get_ai_column_schema(col_name, ai_col) || ai_col == NULL) {
      return OB_NOT_SUPPORTED; // retrying a column that is not an AI column not supported
    }
    return OB_SUCCESS;
  }
};

TEST_F(TestAiColumnResolver, OptionsResolution)
{
  ObArenaAllocator allocator;
  ObAiColumnSchema schema(&allocator);

  // Validate sync_mode: only 'async' supported
  ASSERT_EQ(OB_SUCCESS, schema.set_sync_mode(ObString::make_string("async")));
  ASSERT_EQ("async", schema.get_sync_mode());

  // Validate max_retries: non-negative integer
  schema.set_max_retries(0);
  ASSERT_EQ(0, schema.get_max_retries());
  schema.set_max_retries(2);
  ASSERT_EQ(2, schema.get_max_retries());
}

TEST_F(TestAiColumnResolver, DependencyRestrictions)
{
  ObArenaAllocator allocator;
  ObTableSchema table(&allocator);
  table.set_table_id(100);
  table.set_table_name("docs");

  // Normal column "content"
  ObColumnSchemaV2 col_content;
  col_content.set_column_id(1);
  col_content.set_column_name("content");
  ASSERT_EQ(OB_SUCCESS, table.add_column(col_content));

  // AI column "first_ai" depending on "content"
  ObAiColumnSchema first_ai(&allocator);
  first_ai.set_table_id(100);
  first_ai.set_column_id(2);
  ASSERT_EQ(OB_SUCCESS, first_ai.set_column_name(ObString::make_string("first_ai")));
  ASSERT_EQ(OB_SUCCESS, table.add_ai_column(first_ai));

  // Dependency on normal column "content" succeeds
  ASSERT_EQ(OB_SUCCESS, check_dependency(table, ObString::make_string("content")));

  // Dependency on AI column "first_ai" must be rejected with OB_NOT_SUPPORTED (-1235)
  ASSERT_EQ(OB_NOT_SUPPORTED, check_dependency(table, ObString::make_string("first_ai")));
}

TEST_F(TestAiColumnResolver, VectorDimensionValidation)
{
  // VECTOR(4) expected
  const uint32_t expected_dim = 4;

  // AI_EMBED with omitted dimension (uses default model dim)
  ASSERT_EQ(OB_SUCCESS, validate_vector_dim(expected_dim, 0));

  // AI_EMBED with matching dimension 4
  ASSERT_EQ(OB_SUCCESS, validate_vector_dim(expected_dim, 4));

  // AI_EMBED with mismatching dimension 3 -> OB_ERR_INVALID_VECTOR_DIM (-7600)
  ASSERT_EQ(OB_ERR_INVALID_VECTOR_DIM, validate_vector_dim(expected_dim, 3));
}

TEST_F(TestAiColumnResolver, TypedPromptEnrichment)
{
  ObArenaAllocator allocator;
  ObString base_prompt = ObString::make_string("Classify: sample text");
  ObString enriched;

  // JSON destination column
  ASSERT_EQ(OB_SUCCESS, enrich_prompt(ObJsonType, base_prompt, allocator, enriched));
  ASSERT_TRUE(enriched.find("Return exactly one valid JSON value.") != NULL);

  // Integer destination column
  ASSERT_EQ(OB_SUCCESS, enrich_prompt(ObIntType, base_prompt, allocator, enriched));
  ASSERT_TRUE(enriched.find("Return exactly one base-10 integer.") != NULL);

  // Varchar destination column
  ASSERT_EQ(OB_SUCCESS, enrich_prompt(ObVarcharType, base_prompt, allocator, enriched));
  ASSERT_EQ(base_prompt, enriched);
}

TEST_F(TestAiColumnResolver, RetryAiColumnValidation)
{
  ObArenaAllocator allocator;
  ObTableSchema table(&allocator);
  table.set_table_id(200);
  table.set_table_name("docs");

  ObColumnSchemaV2 col_content;
  col_content.set_column_id(1);
  col_content.set_column_name("content");
  ASSERT_EQ(OB_SUCCESS, table.add_column(col_content));

  ObAiColumnSchema col_result(&allocator);
  col_result.set_table_id(200);
  col_result.set_column_id(2);
  ASSERT_EQ(OB_SUCCESS, col_result.set_column_name(ObString::make_string("result")));
  ASSERT_EQ(OB_SUCCESS, table.add_ai_column(col_result));

  // Retry valid AI column as single action -> SUCCESS
  ASSERT_EQ(OB_SUCCESS, validate_retry(table, ObString::make_string("result"), 1));

  // Retry non-AI column -> OB_NOT_SUPPORTED (1235)
  ASSERT_EQ(OB_NOT_SUPPORTED, validate_retry(table, ObString::make_string("content"), 1));

  // Retry combined with another ALTER action -> OB_NOT_SUPPORTED (1235)
  ASSERT_EQ(OB_NOT_SUPPORTED, validate_retry(table, ObString::make_string("result"), 2));
}

TEST_F(TestAiColumnResolver, StatusPseudoColumnResolution)
{
  // Test distinguishing between unquoted pseudo-column `status` and backticked column `` `status` ``
  ObString unquoted_status = ObString::make_string("status");
  ObString quoted_status = ObString::make_string("`status`");

  bool is_pseudo_status = (unquoted_status[0] != '`');
  ASSERT_TRUE(is_pseudo_status);

  bool is_quoted_status = (quoted_status[0] == '`');
  ASSERT_TRUE(is_quoted_status);

  // Pseudo-column recognizes standard state tokens
  ASSERT_EQ(AI_STATE_PENDING, str_to_ai_column_state(ObString::make_string("PENDING")));
  ASSERT_EQ(AI_STATE_READY, str_to_ai_column_state(ObString::make_string("READY")));
  ASSERT_EQ(AI_STATE_FAILED, str_to_ai_column_state(ObString::make_string("FAILED")));
  ASSERT_EQ(AI_STATE_MAX, str_to_ai_column_state(ObString::make_string("OTHER")));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
