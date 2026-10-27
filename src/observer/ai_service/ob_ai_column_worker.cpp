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

#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ob_ai_column_worker.h"
#include <curl/curl.h>
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/json/ob_json_node.h"
#include "lib/json/ob_json_builder.h"

namespace oceanbase
{
namespace observer
{

using namespace common;
using namespace common::sqlclient;
using namespace share;
using namespace share::schema;

static void escape_sql_str(const common::ObString &src, common::ObStringBuffer &dst)
{
  for (int64_t i = 0; i < src.length(); ++i) {
    char c = src.ptr()[i];
    if (c == '\'') {
      dst.append("''");
    } else {
      dst.append(&c, 1);
    }
  }
}

ObAiColumnWorker::ObAiColumnWorker()
  : is_inited_(false),
    is_stopped_(false)
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
  destroy();
}

int ObAiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else {
    is_stopped_ = false;
    is_inited_ = true;
  }
  return ret;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    if (OB_FAIL(init())) {
      LOG_WARN("fail to init ai column worker", KR(ret));
    }
  }
  if (OB_SUCC(ret)) {
    is_stopped_ = false;
    set_thread_count(1);
    if (OB_FAIL(share::ObThreadPool::start())) {
      LOG_WARN("fail to start ai column worker thread", KR(ret));
    } else {
      LOG_INFO("ai column worker started successfully");
    }
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  is_stopped_ = true;
  share::ObThreadPool::stop();
}

void ObAiColumnWorker::wait()
{
  share::ObThreadPool::wait();
}

void ObAiColumnWorker::destroy()
{
  stop();
  wait();
  share::ObThreadPool::destroy();
  is_inited_ = false;
}

void ObAiColumnWorker::run1()
{
  lib::set_thread_name("AiColWorker");
  while (!is_stopped_) {
    int ret = do_work_();
    if (OB_FAIL(ret)) {
      // Sleep slightly on error to prevent hot spin
      ob_usleep(100000); // 100ms
    } else {
      ob_usleep(50000);  // 50ms polling interval
    }
  }
}

int ObAiColumnWorker::do_work_()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.sql_proxy_) || GCTX.status_ != SS_SERVING) {
    // Server not ready yet
    return OB_SUCCESS;
  }

  ObArenaAllocator allocator(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<AiColumnTaskInfo, 32> tasks;
  if (OB_FAIL(fetch_eligible_task_groups_(allocator, tasks))) {
    LOG_WARN("fail to fetch eligible ai tasks", KR(ret));
  } else if (tasks.empty()) {
    // No tasks to process
  } else {
    // Group tasks by (table_id, request_id, rowkey_pred)
    for (int64_t i = 0; OB_SUCC(ret) && !is_stopped_ && i < tasks.count(); ++i) {
      const AiColumnTaskInfo &task = tasks.at(i);
      // Check if this task was already processed as part of a previous group in this loop
      bool already_handled = false;
      for (int64_t j = 0; j < i; ++j) {
        if (tasks.at(j).table_id_ == task.table_id_ &&
            tasks.at(j).request_id_ == task.request_id_ &&
            tasks.at(j).rowkey_pred_ == task.rowkey_pred_) {
          already_handled = true;
          break;
        }
      }
      if (!already_handled) {
        if (OB_FAIL(process_group_(allocator, task.table_id_, task.request_id_, task.rowkey_pred_, tasks))) {
          LOG_WARN("fail to process ai column task group", KR(ret), K(task.table_id_), K(task.request_id_));
          // Reset ret to continue with other task groups
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::fetch_eligible_task_groups_(ObIAllocator &allocator,
                                                 ObIArray<AiColumnTaskInfo> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt("SELECT task_id, table_id, column_id, request_id, rowkey_predicate, retry_count, state "
                             "FROM oceanbase.__all_ai_column_task "
                             "WHERE state IN (0, 3) AND eligible_ts <= now(6) "
                             "ORDER BY request_id ASC LIMIT 64"))) {
    LOG_WARN("fail to format select task sql", KR(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("fail to execute read ai task query", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          AiColumnTaskInfo task;
          task.task_id_ = 0;
          task.table_id_ = 0;
          task.column_id_ = 0;
          task.request_id_ = 0;
          task.retry_count_ = 0;
          task.state_ = 0;

          int64_t tid = 0;
          int64_t cid = 0;
          ObString pred_str;

          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "task_id", task.task_id_, int64_t, true, true, 0);
          EXTRACT_INT_FIELD_MYSQL(*result, "table_id", tid, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "column_id", cid, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "request_id", task.request_id_, int64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "rowkey_predicate", pred_str);
          EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", task.retry_count_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "state", task.state_, int64_t);

          task.table_id_ = static_cast<uint64_t>(tid);
          task.column_id_ = static_cast<uint64_t>(cid);
          if (OB_SUCC(ret)) {
            if (OB_FAIL(ob_write_string(allocator, pred_str, task.rowkey_pred_))) {
              LOG_WARN("fail to deep copy rowkey predicate", KR(ret));
            } else if (OB_FAIL(tasks.push_back(task))) {
              LOG_WARN("fail to push task", KR(ret));
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::get_table_name_(ObIAllocator &allocator,
                                      uint64_t table_id,
                                      ObString &db_name,
                                      ObString &table_name)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt("SELECT d.database_name, t.table_name "
                             "FROM oceanbase.__all_table t "
                             "JOIN oceanbase.__all_database d ON d.database_id = t.database_id "
                             "WHERE t.table_id = %lu", table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else {
        ObString db_str, tbl_str;
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "database_name", db_str);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "table_name", tbl_str);
        if (OB_SUCC(ret)) {
          if (OB_FAIL(ob_write_string(allocator, db_str, db_name))) {
          } else if (OB_FAIL(ob_write_string(allocator, tbl_str, table_name))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::get_column_name_(ObIAllocator &allocator,
                                       uint64_t table_id,
                                       uint64_t column_id,
                                       ObString &column_name)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt("SELECT column_name FROM oceanbase.__all_column "
                             "WHERE table_id = %lu AND column_id = %lu", table_id, column_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else {
        ObString col_str;
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "column_name", col_str);
        if (OB_SUCC(ret)) {
          ret = ob_write_string(allocator, col_str, column_name);
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::get_ai_column_meta_(ObIAllocator &allocator,
                                          uint64_t table_id,
                                          uint64_t column_id,
                                          AiColumnMetaInfo &meta)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt("SELECT column_name, sync_mode, max_retries, "
                             "state_column_id, request_column_id, result_spec_column_id, "
                             "raw_expr, normalized_expr "
                             "FROM oceanbase.__all_ai_column "
                             "WHERE table_id = %lu AND column_id = %lu", table_id, column_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else {
        meta.table_id_ = table_id;
        meta.column_id_ = column_id;
        int64_t state_cid = 0, req_cid = 0, spec_cid = 0;
        ObString col_str, sync_str, raw_str, norm_str;

        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "column_name", col_str);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "sync_mode", sync_str);
        EXTRACT_INT_FIELD_MYSQL(*result, "max_retries", meta.max_retries_, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "state_column_id", state_cid, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "request_column_id", req_cid, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "result_spec_column_id", spec_cid, int64_t);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "raw_expr", raw_str);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "normalized_expr", norm_str);

        meta.state_column_id_ = static_cast<uint64_t>(state_cid);
        meta.request_column_id_ = static_cast<uint64_t>(req_cid);
        meta.result_spec_column_id_ = static_cast<uint64_t>(spec_cid);

        if (OB_SUCC(ret)) {
          if (OB_FAIL(ob_write_string(allocator, col_str, meta.column_name_))) {
          } else if (OB_FAIL(ob_write_string(allocator, sync_str, meta.sync_mode_))) {
          } else if (OB_FAIL(ob_write_string(allocator, raw_str, meta.raw_expr_))) {
          } else if (OB_FAIL(ob_write_string(allocator, norm_str, meta.normalized_expr_))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::get_endpoint_info_(ObIAllocator &allocator,
                                         const ObString &model_name,
                                         AiEndpointMeta &endpoint)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt("SELECT url, access_key, provider, request_model_name "
                             "FROM oceanbase.__all_ai_model_endpoint "
                             "WHERE ai_model_name = '%.*s' LIMIT 1",
                             model_name.length(), model_name.ptr()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else {
        ObString url_str, key_str, prov_str, req_m_str;
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "url", url_str);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "access_key", key_str);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "provider", prov_str);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "request_model_name", req_m_str);

        if (OB_SUCC(ret)) {
          if (OB_FAIL(ob_write_string(allocator, url_str, endpoint.url_))) {
          } else if (OB_FAIL(ob_write_string(allocator, key_str, endpoint.access_key_))) {
          } else if (OB_FAIL(ob_write_string(allocator, prov_str, endpoint.provider_))) {
          } else if (OB_FAIL(ob_write_string(allocator, req_m_str, endpoint.request_model_name_))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::extract_prompt_expr_(
    const ObString &raw_expr,
    ObString &prompt_expr)
{
  int ret = OB_SUCCESS;
  const char *str = raw_expr.ptr();
  int32_t len = raw_expr.length();
  const char *open_paren = static_cast<const char *>(memchr(str, '(', len));
  if (open_paren == NULL) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *p = open_paren + 1;
    const char *end = str + len;
    bool in_quote = false;
    char quote_char = 0;
    const char *first_comma = NULL;
    while (p < end) {
      if (in_quote) {
        if (*p == quote_char) {
          if (p + 1 < end && *(p + 1) == quote_char) {
            p += 2;
            continue;
          }
          in_quote = false;
        } else if (*p == '\\') {
          p++;
        }
      } else {
        if (*p == '\'' || *p == '"' || *p == '`') {
          in_quote = true;
          quote_char = *p;
        } else if (*p == ',') {
          first_comma = p;
          break;
        }
      }
      p++;
    }

    if (first_comma == NULL) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const char *expr_start = first_comma + 1;
      while (expr_start < end && isspace(*expr_start)) {
        expr_start++;
      }
      p = expr_start;
      int paren_depth = 0;
      in_quote = false;
      const char *expr_end = NULL;
      while (p < end) {
        if (in_quote) {
          if (*p == quote_char) {
            if (p + 1 < end && *(p + 1) == quote_char) {
              p += 2;
              continue;
            }
            in_quote = false;
          } else if (*p == '\\') {
            p++;
          }
        } else {
          if (*p == '\'' || *p == '"' || *p == '`') {
            in_quote = true;
            quote_char = *p;
          } else if (*p == '(') {
            paren_depth++;
          } else if (*p == ')') {
            if (paren_depth == 0) {
              expr_end = p;
              break;
            } else {
              paren_depth--;
            }
          } else if (*p == ',' && paren_depth == 0) {
            expr_end = p;
            break;
          }
        }
        p++;
      }

      if (expr_end == NULL || expr_end <= expr_start) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        while (expr_end > expr_start && isspace(*(expr_end - 1))) {
          expr_end--;
        }
        prompt_expr.assign_ptr(expr_start, static_cast<int32_t>(expr_end - expr_start));
      }
    }
  }
  return ret;
}

size_t ObAiColumnWorker::curl_write_cb_(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t total_size = size * nmemb;
  ObStringBuffer *buf = static_cast<ObStringBuffer *>(userp);
  if (buf != NULL) {
    buf->append(static_cast<const char *>(contents), static_cast<int64_t>(total_size));
  }
  return total_size;
}

int ObAiColumnWorker::call_model_completion_(ObIAllocator &allocator,
                                             const AiEndpointMeta &endpoint,
                                             const ObString &system_prompt,
                                             const ObString &user_text,
                                             ObString &result_content,
                                             int &http_status)
{
  int ret = OB_SUCCESS;
  http_status = 0;
  CURL *curl = curl_easy_init();
  if (OB_ISNULL(curl)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    struct curl_slist *headers = NULL;
    char auth_header[2048] = {0};
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %.*s",
             endpoint.access_key_.length(), endpoint.access_key_.ptr());
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Construct request JSON body:
    // {"model": "<req_model>", "messages": [{"role": "system", "content": "..."}, {"role": "user", "content": "..."}]}
    ObStringBuffer req_body(&allocator);
    req_body.append("{\"model\":\"");
    req_body.append(endpoint.request_model_name_.ptr(), endpoint.request_model_name_.length());
    req_body.append("\",\"messages\":[");
    if (!system_prompt.empty()) {
      req_body.append("{\"role\":\"system\",\"content\":\"");
      // Escape quotes and newlines
      for (int64_t i = 0; i < system_prompt.length(); ++i) {
        char ch = system_prompt.ptr()[i];
        if (ch == '"' || ch == '\\') req_body.append("\\");
        req_body.append(&ch, 1);
      }
      req_body.append("\"},");
    }
    req_body.append("{\"role\":\"user\",\"content\":\"");
    for (int64_t i = 0; i < user_text.length(); ++i) {
      char ch = user_text.ptr()[i];
      if (ch == '"' || ch == '\\') req_body.append("\\");
      req_body.append(&ch, 1);
    }
    req_body.append("\"}]}");

    ObStringBuffer resp_body(&allocator);

    char url_buf[2048] = {0};
    snprintf(url_buf, sizeof(url_buf), "%.*s", endpoint.url_.length(), endpoint.url_.ptr());

    curl_easy_setopt(curl, CURLOPT_URL, url_buf);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body.ptr());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb_);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode curl_res = curl_easy_perform(curl);
    long res_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res_code);
    http_status = static_cast<int>(res_code);

    if (curl_res != CURLE_OK || http_status != 200) {
      // Failed request
      ret = OB_SUCCESS; // Keep ret success, status tells outcome
    } else {
      // Extract content from choices[0].message.content
      // Simple robust JSON extractor for OpenAI choices format
      const char *p = strstr(resp_body.ptr(), "\"content\"");
      if (p != NULL) {
        p = strchr(p, ':');
        if (p != NULL) {
          while (*p == ' ' || *p == '\t' || *p == ':') ++p;
          if (*p == '"') {
            ++p; // Skip opening quote
            const char *start = p;
            ObStringBuffer parsed_str(&allocator);
            while (*p && !(*p == '"' && *(p - 1) != '\\')) {
              if (*p == '\\' && *(p + 1) == '"') {
                parsed_str.append("\"");
                p += 2;
              } else if (*p == '\\' && *(p + 1) == '\\') {
                parsed_str.append("\\");
                p += 2;
              } else if (*p == '\\' && *(p + 1) == 'n') {
                parsed_str.append("\n");
                p += 2;
              } else {
                parsed_str.append(p, 1);
                ++p;
              }
            }
            ObString tmp_res(parsed_str.length(), parsed_str.ptr());
            ret = ob_write_string(allocator, tmp_res, result_content);
          }
        }
      }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }
  return ret;
}

int ObAiColumnWorker::call_model_embedding_(ObIAllocator &allocator,
                                            const AiEndpointMeta &endpoint,
                                            const ObString &user_text,
                                            ObString &result_vector,
                                            int &http_status)
{
  int ret = OB_SUCCESS;
  http_status = 0;
  CURL *curl = curl_easy_init();
  if (OB_ISNULL(curl)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    struct curl_slist *headers = NULL;
    char auth_header[2048] = {0};
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %.*s",
             endpoint.access_key_.length(), endpoint.access_key_.ptr());
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    ObStringBuffer req_body(&allocator);
    req_body.append("{\"model\":\"");
    req_body.append(endpoint.request_model_name_.ptr(), endpoint.request_model_name_.length());
    req_body.append("\",\"input\":[\"");
    for (int64_t i = 0; i < user_text.length(); ++i) {
      char ch = user_text.ptr()[i];
      if (ch == '"' || ch == '\\') req_body.append("\\");
      req_body.append(&ch, 1);
    }
    req_body.append("\"]}");

    ObStringBuffer resp_body(&allocator);

    char url_buf[2048] = {0};
    snprintf(url_buf, sizeof(url_buf), "%.*s", endpoint.url_.length(), endpoint.url_.ptr());

    curl_easy_setopt(curl, CURLOPT_URL, url_buf);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body.ptr());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb_);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode curl_res = curl_easy_perform(curl);
    long res_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res_code);
    http_status = static_cast<int>(res_code);

    if (curl_res != CURLE_OK || http_status != 200) {
      ret = OB_SUCCESS;
    } else {
      // Extract data[0].embedding array, format as [x, y, z, ...]
      const char *p = strstr(resp_body.ptr(), "\"embedding\"");
      if (p != NULL) {
        p = strchr(p, '[');
        if (p != NULL) {
          const char *end = strchr(p, ']');
          if (end != NULL) {
            ObString vec_slice(static_cast<int32_t>(end - p + 1), p);
            ret = ob_write_string(allocator, vec_slice, result_vector);
          }
        }
      }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }
  return ret;
}

int ObAiColumnWorker::process_group_(ObIAllocator &allocator,
                                     const uint64_t table_id,
                                     const int64_t request_id,
                                     const ObString &rowkey_pred,
                                     const ObIArray<AiColumnTaskInfo> &all_tasks)
{
  int ret = OB_SUCCESS;

  // Escape single quotes for embedding in SQL string literals
  ObStringBuffer escaped_pred(&allocator);
  escape_sql_str(rowkey_pred, escaped_pred);

  // 1. Resolve table name and database name
  ObString db_name, tbl_name;
  if (OB_FAIL(get_table_name_(allocator, table_id, db_name, tbl_name))) {
    LOG_WARN("fail to get table name", KR(ret), K(table_id));
    return ret;
  }

  // 2. Collect all tasks in this group
  ObSEArray<AiColumnTaskInfo, 8> group_tasks;
  ObSqlString group_tasks_sql;
  if (OB_FAIL(group_tasks_sql.assign_fmt(
      "SELECT task_id, table_id, column_id, request_id, rowkey_predicate, retry_count, state "
      "FROM oceanbase.__all_ai_column_task "
      "WHERE table_id = %lu AND request_id = %ld AND rowkey_predicate = '%.*s'",
      table_id, request_id,
      static_cast<int>(escaped_pred.length()), escaped_pred.ptr()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, group_tasks_sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          AiColumnTaskInfo task;
          ObString pred_str;
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "task_id", task.task_id_, int64_t, true, true, 0);
          EXTRACT_INT_FIELD_MYSQL(*result, "table_id", task.table_id_, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "column_id", task.column_id_, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "request_id", task.request_id_, int64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "rowkey_predicate", pred_str);
          EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", task.retry_count_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "state", task.state_, int64_t);
          if (OB_SUCC(ret)) {
            if (OB_FAIL(ob_write_string(allocator, pred_str, task.rowkey_pred_))) {
            } else if (OB_FAIL(group_tasks.push_back(task))) {
            }
          }
        }
        if (ret == OB_ITER_END) {
          ret = OB_SUCCESS;
        }
      }
    }
  }

  if (group_tasks.empty()) {
    for (int64_t i = 0; i < all_tasks.count(); ++i) {
      if (all_tasks.at(i).table_id_ == table_id &&
          all_tasks.at(i).request_id_ == request_id &&
          all_tasks.at(i).rowkey_pred_ == rowkey_pred) {
        group_tasks.push_back(all_tasks.at(i));
      }
    }
  }

  if (group_tasks.empty()) {
    return OB_SUCCESS;
  }

  // 3. Check current row in table to verify stale request guard
  // We need to fetch the request column id for one of the AI columns
  AiColumnMetaInfo first_meta;
  if (OB_FAIL(get_ai_column_meta_(allocator, table_id, group_tasks.at(0).column_id_, first_meta))) {
    LOG_WARN("fail to get ai column meta", KR(ret));
    return ret;
  }

  ObString req_col_name;
  if (OB_FAIL(get_column_name_(allocator, table_id, first_meta.request_column_id_, req_col_name))) {
    LOG_WARN("fail to get request column name", KR(ret));
    return ret;
  }

  // Fetch current request_id and row from table
  int64_t current_row_req_id = -1;
  ObSqlString check_sql;
  if (OB_FAIL(check_sql.assign_fmt("SELECT `%.*s` FROM `%.*s`.`%.*s` WHERE %.*s",
                                   req_col_name.length(), req_col_name.ptr(),
                                   db_name.length(), db_name.ptr(),
                                   tbl_name.length(), tbl_name.ptr(),
                                   rowkey_pred.length(), rowkey_pred.ptr()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, check_sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (ret == OB_ITER_END) {
          // Row was deleted! Drop tasks.
          ret = OB_SUCCESS;
          ObSqlString del_sql;
          del_sql.assign_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND rowkey_predicate = '%.*s'",
                             table_id,
                             static_cast<int>(escaped_pred.length()), escaped_pred.ptr());
          int64_t affected = 0;
          GCTX.sql_proxy_->write(del_sql.ptr(), affected);
          return OB_SUCCESS;
        }
      } else {
        EXTRACT_INT_FIELD_MYSQL(*result, req_col_name.ptr(), current_row_req_id, int64_t);
      }
    }
  }

  if (OB_FAIL(ret)) {
    return ret;
  }

  // Stale guard: if current row's request id is greater than task request id, discard this task!
  if (current_row_req_id > request_id) {
    LOG_INFO("discarding stale ai column tasks", K(request_id), K(current_row_req_id));
    ObSqlString del_sql;
    del_sql.assign_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND request_id = %ld AND rowkey_predicate = '%.*s'",
                       table_id, request_id,
                       static_cast<int>(escaped_pred.length()), escaped_pred.ptr());
    int64_t affected = 0;
    GCTX.sql_proxy_->write(del_sql.ptr(), affected);
    return OB_SUCCESS;
  }

  // 4. Execute model calls for all columns in this group
  struct TaskExecutionResult
  {
    AiColumnTaskInfo task_;
    AiColumnMetaInfo meta_;
    ObString state_col_name_;
    ObString result_col_name_;
    ObString result_val_;
    int64_t final_state_; // 2 (READY), 3 (RETRY), 4 (FAILED)
  };

  ObSEArray<TaskExecutionResult, 8> exec_results;
  bool all_terminal = true;

  for (int64_t i = 0; OB_SUCC(ret) && i < group_tasks.count(); ++i) {
    TaskExecutionResult exec_res;
    exec_res.task_ = group_tasks.at(i);
    if (OB_FAIL(get_ai_column_meta_(allocator, table_id, exec_res.task_.column_id_, exec_res.meta_))) {
      LOG_WARN("fail to get ai column meta", KR(ret));
    } else if (OB_FAIL(get_column_name_(allocator, table_id, exec_res.meta_.state_column_id_, exec_res.state_col_name_))) {
      LOG_WARN("fail to get state column name", KR(ret));
    } else if (OB_FAIL(get_column_name_(allocator, table_id, exec_res.meta_.column_id_, exec_res.result_col_name_))) {
      LOG_WARN("fail to get result column name", KR(ret));
    } else {
      // Parse model name from raw_expr: e.g. AI_COMPLETE('p0_worker_complete', ...) or AI_EMBED('p0_vector_embed', ...)
      bool is_embed = (strstr(exec_res.meta_.raw_expr_.ptr(), "AI_EMBED") != NULL ||
                       strstr(exec_res.meta_.raw_expr_.ptr(), "ai_embed") != NULL);

      // Extract model_name (quoted or unquoted identifier)
      ObString model_name;
      const char *raw_ptr = exec_res.meta_.raw_expr_.ptr();
      const char *open_paren = strchr(raw_ptr, '(');
      if (open_paren != NULL) {
        const char *p = open_paren + 1;
        while (*p && isspace(*p)) p++;
        if (*p == '\'' || *p == '"') {
          char q = *p++;
          const char *m_start = p;
          while (*p && *p != q) p++;
          model_name.assign_ptr(m_start, static_cast<int32_t>(p - m_start));
        } else {
          const char *m_start = p;
          while (*p && *p != ',' && *p != ')' && !isspace(*p)) p++;
          model_name.assign_ptr(m_start, static_cast<int32_t>(p - m_start));
        }
      }

      AiEndpointMeta endpoint;
      if (OB_FAIL(get_endpoint_info_(allocator, model_name, endpoint))) {
        LOG_WARN("fail to get endpoint info", KR(ret), K(model_name));
      } else {
        ObString prompt_expr;
        if (OB_FAIL(extract_prompt_expr_(exec_res.meta_.raw_expr_, prompt_expr))) {
          LOG_WARN("fail to extract prompt expr", KR(ret), K(exec_res.meta_.raw_expr_));
        } else {
          ObString evaluated_user_text;
          ObSqlString read_prompt_sql;
          if (OB_FAIL(read_prompt_sql.assign_fmt(
              "SELECT %.*s AS __ai_prompt FROM `%.*s`.`%.*s` WHERE %.*s",
              prompt_expr.length(), prompt_expr.ptr(),
              db_name.length(), db_name.ptr(),
              tbl_name.length(), tbl_name.ptr(),
              rowkey_pred.length(), rowkey_pred.ptr()))) {
          } else {
            SMART_VAR(ObMySQLProxy::MySQLResult, res) {
              ObMySQLResult *result = NULL;
              if (OB_FAIL(GCTX.sql_proxy_->read(res, read_prompt_sql.ptr()))) {
                LOG_WARN("fail to evaluate prompt expr via sql", KR(ret), K(read_prompt_sql));
              } else if (OB_ISNULL(result = res.get_result())) {
                ret = OB_ERR_UNEXPECTED;
              } else if (OB_FAIL(result->next())) {
                LOG_WARN("fail to fetch evaluated prompt row", KR(ret));
              } else {
                ObString val_str;
                int get_ret = result->get_varchar("__ai_prompt", val_str);
                if (OB_ERR_NULL_VALUE == get_ret) {
                  exec_res.final_state_ = 4; // AI_STATE_FAILED
                  exec_res.result_val_.reset();
                  exec_results.push_back(exec_res);
                  continue;
                } else if (OB_FAIL(get_ret)) {
                  LOG_WARN("fail to extract prompt value", KR(ret), KR(get_ret));
                } else {
                  ob_write_string(allocator, val_str, evaluated_user_text);
                }
              }
            }
          }

          if (OB_SUCC(ret)) {
            ObString sys_prompt;
            if (strstr(exec_res.meta_.normalized_expr_.ptr(), "valid JSON value") != NULL) {
              sys_prompt = ObString::make_string("Return exactly one valid JSON value.");
            } else if (strstr(exec_res.meta_.normalized_expr_.ptr(), "base-10 integer") != NULL) {
              sys_prompt = ObString::make_string("Return exactly one base-10 integer.");
            }

            int http_status = 0;
            ObString model_res;
            if (is_embed) {
              ret = call_model_embedding_(allocator, endpoint, evaluated_user_text, model_res, http_status);
            } else {
              ret = call_model_completion_(allocator, endpoint, sys_prompt, evaluated_user_text, model_res, http_status);
            }

            if (http_status == 200 && !model_res.empty()) {
              exec_res.final_state_ = 2; // AI_STATE_READY
              exec_res.result_val_ = model_res;
            } else {
              if (exec_res.task_.retry_count_ < exec_res.meta_.max_retries_) {
                exec_res.final_state_ = 3; // AI_STATE_RETRY
                all_terminal = false;
                // Update retry state in task table
                ObSqlString upd_task_sql;
                upd_task_sql.assign_fmt("UPDATE oceanbase.__all_ai_column_task "
                                        "SET state = 3, retry_count = retry_count + 1, "
                                        "eligible_ts = date_add(now(6), interval 100 millisecond) "
                                        "WHERE table_id = %lu AND column_id = %lu AND rowkey_predicate = '%.*s'",
                                        table_id, exec_res.task_.column_id_,
                                        static_cast<int>(escaped_pred.length()), escaped_pred.ptr());
                int64_t affected = 0;
                GCTX.sql_proxy_->write(upd_task_sql.ptr(), affected);
              } else {
                // Terminal failure
                exec_res.final_state_ = 4; // AI_STATE_FAILED
                exec_res.result_val_.reset();
              }
            }
            exec_results.push_back(exec_res);
          }
        }
      }
    }
  }

  // 5. Grouped Publishing: If and only if ALL tasks in this group reached terminal state (READY or FAILED),
  // publish results atomically in one transaction!
  if (OB_SUCC(ret) && all_terminal && exec_results.count() == group_tasks.count()) {
    ObMySQLTransaction trans;
    if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
      LOG_WARN("fail to start writeback trans", KR(ret));
    } else {
      // Verify stale guard inside transaction
      int64_t trans_check_req_id = -1;
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        ObMySQLResult *result = NULL;
        if (OB_FAIL(trans.read(res, check_sql.ptr()))) {
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result->next())) {
          // Row was deleted
        } else {
          EXTRACT_INT_FIELD_MYSQL(*result, req_col_name.ptr(), trans_check_req_id, int64_t);
        }
      }

      if (OB_SUCC(ret)) {
        if (trans_check_req_id == request_id) {
          // Construct writeback UPDATE statement for target row
          ObSqlString update_sql;
          update_sql.assign_fmt("UPDATE `%.*s`.`%.*s` SET ",
                                db_name.length(), db_name.ptr(),
                                tbl_name.length(), tbl_name.ptr());
          for (int64_t i = 0; i < exec_results.count(); ++i) {
            const TaskExecutionResult &r = exec_results.at(i);
            if (i > 0) {
              update_sql.append(", ");
            }
            if (r.final_state_ == 2) { // READY
              // Escape single quotes in result
              ObStringBuffer escaped_val(&allocator);
              escape_sql_str(r.result_val_, escaped_val);
              update_sql.append_fmt("`%.*s` = '%.*s', `%.*s` = %ld",
                                    r.result_col_name_.length(), r.result_col_name_.ptr(),
                                    static_cast<int>(escaped_val.length()), escaped_val.ptr(),
                                    r.state_col_name_.length(), r.state_col_name_.ptr(),
                                    r.final_state_);
            } else {
              // FAILED (state = 4, result = NULL)
              update_sql.append_fmt("`%.*s` = NULL, `%.*s` = %ld",
                                    r.result_col_name_.length(), r.result_col_name_.ptr(),
                                    r.state_col_name_.length(), r.state_col_name_.ptr(),
                                    r.final_state_);
            }
          }
          update_sql.append_fmt(" WHERE %.*s", rowkey_pred.length(), rowkey_pred.ptr());

          int64_t affected_rows = 0;
          if (OB_FAIL(trans.write(update_sql.ptr(), affected_rows))) {
            LOG_WARN("fail to execute writeback update", KR(ret), K(update_sql));
          }

          // Delete group tasks from oceanbase.__all_ai_column_task
          if (OB_SUCC(ret)) {
            ObSqlString del_task_sql;
            del_task_sql.assign_fmt("DELETE FROM oceanbase.__all_ai_column_task "
                                    "WHERE table_id = %lu AND request_id = %ld AND rowkey_predicate = '%.*s'",
                                    table_id, request_id,
                                    static_cast<int>(escaped_pred.length()), escaped_pred.ptr());
            if (OB_FAIL(trans.write(del_task_sql.ptr(), affected_rows))) {
              LOG_WARN("fail to delete completed ai tasks", KR(ret), K(del_task_sql));
            }
          }
        } else {
          // Row was deleted or updated with a newer request_id while worker was processing.
          // Discard and delete stale tasks inside the transaction.
          LOG_INFO("discarding stale ai column tasks in writeback trans", K(request_id), K(trans_check_req_id));
          ObSqlString del_task_sql;
          del_task_sql.assign_fmt("DELETE FROM oceanbase.__all_ai_column_task "
                                  "WHERE table_id = %lu AND request_id = %ld AND rowkey_predicate = '%.*s'",
                                  table_id, request_id,
                                  static_cast<int>(escaped_pred.length()), escaped_pred.ptr());
          int64_t affected_rows = 0;
          if (OB_FAIL(trans.write(del_task_sql.ptr(), affected_rows))) {
            LOG_WARN("fail to delete stale ai tasks", KR(ret), K(del_task_sql));
          }
        }
      }

      int end_ret = trans.end(OB_SUCC(ret));
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("fail to end transaction", K(end_ret));
        ret = OB_SUCC(ret) ? end_ret : ret;
      }
    }
  }

  return ret;
}

} // namespace observer
} // namespace oceanbase
