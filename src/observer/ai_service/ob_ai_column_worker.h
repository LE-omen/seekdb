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

#ifndef OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_

#include "lib/ob_define.h"
#include "share/ob_thread_pool.h"
#include "share/schema/ob_ai_column_schema.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
class ObISQLClient;
}
namespace observer
{

struct AiColumnTaskInfo
{
  int64_t task_id_;
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t request_id_;
  common::ObString rowkey_pred_;
  int64_t retry_count_;
  int64_t state_;
};

struct AiColumnMetaInfo
{
  uint64_t table_id_;
  uint64_t column_id_;
  common::ObString column_name_;
  common::ObString sync_mode_;
  int64_t max_retries_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  common::ObString raw_expr_;
  common::ObString normalized_expr_;
};

struct AiEndpointMeta
{
  common::ObString url_;
  common::ObString access_key_;
  common::ObString provider_;
  common::ObString request_model_name_;
};

class ObAiColumnWorker : public share::ObThreadPool
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker();

  int init();
  int start();
  void stop();
  void wait();
  void destroy();

protected:
  void run1() override;

private:
  int do_work_();
  int fetch_eligible_task_groups_(common::ObIAllocator &allocator,
                                  common::ObIArray<AiColumnTaskInfo> &tasks);
  int process_group_(common::ObIAllocator &allocator,
                     const uint64_t table_id,
                     const int64_t request_id,
                     const common::ObString &rowkey_pred,
                     const common::ObIArray<AiColumnTaskInfo> &all_tasks);
  int get_table_name_(common::ObIAllocator &allocator,
                      uint64_t table_id,
                      common::ObString &db_name,
                      common::ObString &table_name);
  int get_column_name_(common::ObIAllocator &allocator,
                       uint64_t table_id,
                       uint64_t column_id,
                       common::ObString &column_name);
  int get_ai_column_meta_(common::ObIAllocator &allocator,
                          uint64_t table_id,
                          uint64_t column_id,
                          AiColumnMetaInfo &meta);
  int get_endpoint_info_(common::ObIAllocator &allocator,
                         const common::ObString &model_name,
                         AiEndpointMeta &endpoint);
  int extract_prompt_expr_(const common::ObString &raw_expr,
                           common::ObString &prompt_expr);
  int call_model_completion_(common::ObIAllocator &allocator,
                             const AiEndpointMeta &endpoint,
                             const common::ObString &system_prompt,
                             const common::ObString &user_text,
                             common::ObString &result_content,
                             int &http_status);
  int call_model_embedding_(common::ObIAllocator &allocator,
                            const AiEndpointMeta &endpoint,
                            const common::ObString &user_text,
                            common::ObString &result_vector,
                            int &http_status);
  static size_t curl_write_cb_(void *contents, size_t size, size_t nmemb, void *userp);

private:
  bool is_inited_;
  volatile bool is_stopped_;
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_AI_SERVICE_OB_AI_COLUMN_WORKER_H_
