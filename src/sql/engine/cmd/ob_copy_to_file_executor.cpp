/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/cmd/ob_copy_to_file_executor.h"
#include "sql/resolver/cmd/ob_copy_to_file_stmt.h"
#include "sql/engine/ob_exec_context.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_isql_client.h"

namespace oceanbase
{
namespace sql
{

using namespace common;

int ObCopyToFileExecutor::execute(ObExecContext &ctx, ObCopyToFileStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  common::sqlclient::ObISQLConnection *conn = NULL;
  query::ObInnerSQLConnectionGuard conn_guard;

  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is null", K(ret));
  } else if (!session->is_file_sql_enabled()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy to file when enable_file_sql is disabled");
  } else if (OB_FAIL(
                 query::ObInnerSQLConnectionAccess::
                     create_spi_connection_with_external_session(
                         session, conn_guard))) {
    LOG_WARN("failed to create spi connection", K(ret));
  } else if (OB_ISNULL(conn = conn_guard.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("conn is null", K(ret));
  } else {
    ObISQLClient::ReadResult res;
    if (OB_FAIL(conn->execute_read(stmt.get_select_sql(), res))) {
      LOG_WARN("failed to execute read query for copy", K(ret), K(stmt.get_select_sql()));
    } else {
      common::sqlclient::ObMySQLResult *result = res.get_result();
      if (OB_ISNULL(result)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", K(ret));
      } else if (OB_FAIL(ObFileScanUtils::export_to_csv(stmt.get_file_path(), stmt.get_options(), stmt.get_column_names(), result))) {
        LOG_WARN("failed to export query result to csv", K(ret), K(stmt.get_file_path()));
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
