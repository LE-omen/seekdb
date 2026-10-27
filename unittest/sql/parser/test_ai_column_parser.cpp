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

class TestAiColumnParser : public ::testing::Test
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

TEST_F(TestAiColumnParser, CreateTableWithAiColumnCompletion)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "CREATE TABLE docs (\n"
                    "  id BIGINT PRIMARY KEY,\n"
                    "  content VARCHAR(200),\n"
                    "  tags JSON AI COLUMN (\n"
                    "    AI_COMPLETE('p0_ddl_complete', CONCAT('Classify: ', content))\n"
                    "  ) WITH (sync_mode='async', max_retries=2)\n"
                    ");";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ASSERT_EQ(T_STMT_LIST, result.result_tree_->type_);
  ASSERT_GT(result.result_tree_->num_child_, 0);

  ParseNode *create_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(create_stmt != NULL);
  ASSERT_EQ(T_CREATE_TABLE, create_stmt->type_);

  // Locate the table element list
  ParseNode *table_elements = create_stmt->children_[1]; // table_element_list
  ASSERT_TRUE(table_elements != NULL);

  // Scan children for AI column definition
  bool found_ai_col = false;
  for (int32_t i = 0; i < table_elements->num_child_; ++i) {
    ParseNode *elem = table_elements->children_[i];
    if (elem != NULL && elem->type_ == T_COLUMN_DEFINITION && elem->num_child_ >= 5) {
      ParseNode *ai_marker = elem->children_[4];
      if (ai_marker != NULL && ai_marker->type_ == T_AI_COLUMN) {
        found_ai_col = true;
        // Verify column name is tags
        ParseNode *col_ref = elem->children_[0];
        ASSERT_TRUE(col_ref != NULL);
        ParseNode *col_name = col_ref->children_[0];
        ASSERT_TRUE(col_name != NULL);
        ASSERT_EQ("tags", ObString(col_name->str_len_, col_name->str_value_));

        // Verify options node
        if (elem->num_child_ >= 8 && elem->children_[7] != NULL) {
          ParseNode *opt_node = elem->children_[7];
          ASSERT_EQ(T_AI_COLUMN_OPTIONS, opt_node->type_);
        }
      }
    }
  }
  ASSERT_TRUE(found_ai_col);
}

TEST_F(TestAiColumnParser, CreateTableWithAiColumnEmbedding)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "CREATE TABLE docs (\n"
                    "  id BIGINT PRIMARY KEY,\n"
                    "  content VARCHAR(200),\n"
                    "  emb VECTOR(4) AI COLUMN (\n"
                    "    AI_EMBED('p0_ddl_embed', content)\n"
                    "  ) WITH (sync_mode='async', max_retries=3)\n"
                    ");";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);

  ParseNode *create_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(create_stmt != NULL);
  ASSERT_EQ(T_CREATE_TABLE, create_stmt->type_);
}

TEST_F(TestAiColumnParser, AlterTableAddAiColumn)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "ALTER TABLE docs ADD COLUMN tags JSON AI COLUMN (\n"
                    "  AI_COMPLETE('p0_ddl_complete', content)\n"
                    ") WITH (sync_mode='async', max_retries=1);";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ParseNode *alter_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(alter_stmt != NULL);
  ASSERT_EQ(T_ALTER_TABLE, alter_stmt->type_);
}

TEST_F(TestAiColumnParser, AlterTableModifyAiColumn)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "ALTER TABLE docs MODIFY COLUMN tags JSON AI COLUMN (\n"
                    "  AI_COMPLETE('p0_ddl_complete', content)\n"
                    ") WITH (sync_mode='async', max_retries=2);";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ParseNode *alter_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(alter_stmt != NULL);
  ASSERT_EQ(T_ALTER_TABLE, alter_stmt->type_);
}

TEST_F(TestAiColumnParser, AlterTableRetryAiColumnWithPredicate)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "ALTER TABLE docs RETRY AI COLUMN tags WHERE status = 'FAILED';";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ParseNode *alter_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(alter_stmt != NULL);
  ASSERT_EQ(T_ALTER_TABLE, alter_stmt->type_);

  // Verify alter table action is T_AI_COLUMN_RETRY
  ParseNode *alter_action_list = alter_stmt->children_[1];
  ASSERT_TRUE(alter_action_list != NULL);
  ParseNode *action = alter_action_list->children_[0];
  ASSERT_TRUE(action != NULL);
  ASSERT_EQ(T_AI_COLUMN_RETRY, action->type_);

  // Target column name
  ParseNode *target_col = action->children_[0];
  ASSERT_TRUE(target_col != NULL);
  ASSERT_EQ("tags", ObString(target_col->str_len_, target_col->str_value_));

  // Where condition
  ParseNode *where_cond = action->children_[1];
  ASSERT_TRUE(where_cond != NULL);
}

TEST_F(TestAiColumnParser, AlterTableRetryAiColumnUnconditional)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "ALTER TABLE docs RETRY AI COLUMN tags;";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ParseNode *alter_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(alter_stmt != NULL);
  ASSERT_EQ(T_ALTER_TABLE, alter_stmt->type_);

  ParseNode *alter_action_list = alter_stmt->children_[1];
  ASSERT_TRUE(alter_action_list != NULL);
  ParseNode *action = alter_action_list->children_[0];
  ASSERT_TRUE(action != NULL);
  ASSERT_EQ(T_AI_COLUMN_RETRY, action->type_);

  // Target column name
  ParseNode *target_col = action->children_[0];
  ASSERT_TRUE(target_col != NULL);
  ASSERT_EQ("tags", ObString(target_col->str_len_, target_col->str_value_));

  // No where condition
  ParseNode *where_cond = action->children_[1];
  ASSERT_TRUE(where_cond == NULL);
}

TEST_F(TestAiColumnParser, ShowAiColumns)
{
  ObArenaAllocator allocator;
  ParseResult result;
  const char *sql = "SHOW AI COLUMNS FROM docs;";

  int ret = parse(sql, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ParseNode *show_stmt = result.result_tree_->children_[0];
  ASSERT_TRUE(show_stmt != NULL);
  ASSERT_EQ(T_SHOW_AI_COLUMNS, show_stmt->type_);

  // IN variant
  const char *sql2 = "SHOW AI COLUMNS IN docs;";
  int ret2 = parse(sql2, result, allocator);
  ASSERT_EQ(OB_SUCCESS, ret2);
  ASSERT_TRUE(result.result_tree_ != NULL);
  ParseNode *show_stmt2 = result.result_tree_->children_[0];
  ASSERT_TRUE(show_stmt2 != NULL);
  ASSERT_EQ(T_SHOW_AI_COLUMNS, show_stmt2->type_);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
