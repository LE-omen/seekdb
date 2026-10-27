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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_

#include "share/schema/ob_schema_struct.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

enum ObAiColumnState
{
  AI_STATE_PENDING = 0,
  AI_STATE_RUNNING = 1,
  AI_STATE_READY = 2,
  AI_STATE_RETRY = 3,
  AI_STATE_FAILED = 4,
  AI_STATE_MAX
};

inline const char *ai_column_state_to_str(ObAiColumnState state)
{
  switch (state) {
    case AI_STATE_PENDING: return "PENDING";
    case AI_STATE_RUNNING: return "RUNNING";
    case AI_STATE_READY: return "READY";
    case AI_STATE_RETRY: return "RETRY";
    case AI_STATE_FAILED: return "FAILED";
    default: return "UNKNOWN";
  }
}

inline ObAiColumnState str_to_ai_column_state(const ObString &str)
{
  if (0 == str.case_compare("PENDING")) return AI_STATE_PENDING;
  if (0 == str.case_compare("RUNNING")) return AI_STATE_RUNNING;
  if (0 == str.case_compare("READY")) return AI_STATE_READY;
  if (0 == str.case_compare("RETRY")) return AI_STATE_RETRY;
  if (0 == str.case_compare("FAILED")) return AI_STATE_FAILED;
  return AI_STATE_MAX;
}

class ObAiColumnSchema : public ObSchema
{
  OB_UNIS_VERSION(1);
public:
  ObAiColumnSchema();
  explicit ObAiColumnSchema(common::ObIAllocator *allocator);
  virtual ~ObAiColumnSchema() = default;

  void reset();
  bool is_valid() const;
  int assign(const ObAiColumnSchema &other);
  int64_t get_convert_size() const override;

  // Remap column id (e.g. during schema copy, table recreate, column re-indexing)
  void remap_column_id(uint64_t old_id, uint64_t new_id);

  // Check if given column_id is one of the 3 hidden tracking columns
  bool is_hidden_column(uint64_t col_id) const;

  // Getters
  OB_INLINE uint64_t get_table_id() const { return table_id_; }
  OB_INLINE uint64_t get_column_id() const { return column_id_; }
  OB_INLINE const ObString &get_column_name() const { return column_name_; }
  OB_INLINE const ObString &get_sync_mode() const { return sync_mode_; }
  OB_INLINE int64_t get_max_retries() const { return max_retries_; }
  OB_INLINE uint64_t get_state_column_id() const { return state_column_id_; }
  OB_INLINE uint64_t get_request_column_id() const { return request_column_id_; }
  OB_INLINE uint64_t get_result_spec_column_id() const { return result_spec_column_id_; }
  OB_INLINE const ObString &get_raw_expr() const { return raw_expr_; }
  OB_INLINE const ObString &get_normalized_expr() const { return normalized_expr_; }

  // Setters
  OB_INLINE void set_table_id(const uint64_t table_id) { table_id_ = table_id; }
  OB_INLINE void set_column_id(const uint64_t column_id) { column_id_ = column_id; }
  OB_INLINE int set_column_name(const ObString &name) { return deep_copy_str(name, column_name_); }
  OB_INLINE int set_sync_mode(const ObString &sync_mode) { return deep_copy_str(sync_mode, sync_mode_); }
  OB_INLINE void set_max_retries(const int64_t retries) { max_retries_ = retries; }
  OB_INLINE void set_state_column_id(const uint64_t col_id) { state_column_id_ = col_id; }
  OB_INLINE void set_request_column_id(const uint64_t col_id) { request_column_id_ = col_id; }
  OB_INLINE void set_result_spec_column_id(const uint64_t col_id) { result_spec_column_id_ = col_id; }
  OB_INLINE int set_raw_expr(const ObString &expr) { return deep_copy_str(expr, raw_expr_); }
  OB_INLINE int set_normalized_expr(const ObString &expr) { return deep_copy_str(expr, normalized_expr_); }

  TO_STRING_KV(K_(table_id),
               K_(column_id),
               K_(column_name),
               K_(sync_mode),
               K_(max_retries),
               K_(state_column_id),
               K_(request_column_id),
               K_(result_spec_column_id),
               K_(raw_expr),
               K_(normalized_expr));

private:
  uint64_t table_id_;
  uint64_t column_id_;
  ObString column_name_;
  ObString sync_mode_;
  int64_t max_retries_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  ObString raw_expr_;
  ObString normalized_expr_;
};

} // namespace schema
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_
