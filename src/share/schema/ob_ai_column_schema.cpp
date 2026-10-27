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

#define USING_LOG_PREFIX SHARE_SCHEMA
#include "share/schema/ob_ai_column_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

OB_SERIALIZE_MEMBER(ObAiColumnSchema,
                    table_id_,
                    column_id_,
                    column_name_,
                    sync_mode_,
                    max_retries_,
                    state_column_id_,
                    request_column_id_,
                    result_spec_column_id_,
                    raw_expr_,
                    normalized_expr_);

ObAiColumnSchema::ObAiColumnSchema()
  : ObSchema()
{
  reset();
}

ObAiColumnSchema::ObAiColumnSchema(common::ObIAllocator *allocator)
  : ObSchema(allocator)
{
  reset();
}

void ObAiColumnSchema::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  column_name_.reset();
  sync_mode_.reset();
  max_retries_ = 2;
  state_column_id_ = OB_INVALID_ID;
  request_column_id_ = OB_INVALID_ID;
  result_spec_column_id_ = OB_INVALID_ID;
  raw_expr_.reset();
  normalized_expr_.reset();
}

bool ObAiColumnSchema::is_valid() const
{
  return table_id_ != OB_INVALID_ID
      && column_id_ != OB_INVALID_ID
      && !column_name_.empty();
}

int ObAiColumnSchema::assign(const ObAiColumnSchema &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    reset();
    table_id_ = other.table_id_;
    column_id_ = other.column_id_;
    max_retries_ = other.max_retries_;
    state_column_id_ = other.state_column_id_;
    request_column_id_ = other.request_column_id_;
    result_spec_column_id_ = other.result_spec_column_id_;

    if (OB_FAIL(deep_copy_str(other.column_name_, column_name_))) {
      LOG_WARN("fail to deep copy column_name", K(ret));
    } else if (OB_FAIL(deep_copy_str(other.sync_mode_, sync_mode_))) {
      LOG_WARN("fail to deep copy sync_mode", K(ret));
    } else if (OB_FAIL(deep_copy_str(other.raw_expr_, raw_expr_))) {
      LOG_WARN("fail to deep copy raw_expr", K(ret));
    } else if (OB_FAIL(deep_copy_str(other.normalized_expr_, normalized_expr_))) {
      LOG_WARN("fail to deep copy normalized_expr", K(ret));
    }
  }
  return ret;
}

int64_t ObAiColumnSchema::get_convert_size() const
{
  return sizeof(*this)
      + column_name_.length() + 1
      + sync_mode_.length() + 1
      + raw_expr_.length() + 1
      + normalized_expr_.length() + 1;
}

void ObAiColumnSchema::remap_column_id(uint64_t old_id, uint64_t new_id)
{
  if (column_id_ == old_id) {
    column_id_ = new_id;
  }
  if (state_column_id_ == old_id) {
    state_column_id_ = new_id;
  }
  if (request_column_id_ == old_id) {
    request_column_id_ = new_id;
  }
  if (result_spec_column_id_ == old_id) {
    result_spec_column_id_ = new_id;
  }
}

bool ObAiColumnSchema::is_hidden_column(uint64_t col_id) const
{
  return col_id == state_column_id_
      || col_id == request_column_id_
      || col_id == result_spec_column_id_;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
