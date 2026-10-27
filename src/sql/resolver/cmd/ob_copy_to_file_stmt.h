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

#ifndef OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_FILE_STMT_H_
#define OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_FILE_STMT_H_

#include "sql/resolver/cmd/ob_cmd_stmt.h"
#include "lib/string/ob_string.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

class ObCopyToFileStmt : public ObCMDStmt
{
public:
  ObCopyToFileStmt()
    : ObCMDStmt(stmt::T_COPY_TO_FILE),
      select_sql_(),
      file_path_(),
      options_()
  {}
  virtual ~ObCopyToFileStmt() {}

  void set_select_sql(const common::ObString &sql) { select_sql_ = sql; }
  const common::ObString &get_select_sql() const { return select_sql_; }

  void set_file_path(const common::ObString &path) { file_path_ = path; }
  const common::ObString &get_file_path() const { return file_path_; }

  void set_options(const FileScanOptions &opt) { options_ = opt; }
  const FileScanOptions &get_options() const { return options_; }

  common::ObIArray<common::ObString> &get_column_names() { return column_names_; }
  const common::ObIArray<common::ObString> &get_column_names() const { return column_names_; }
  int add_column_name(const common::ObString &name) { return column_names_.push_back(name); }

  DECLARE_VIRTUAL_TO_STRING;

private:
  common::ObString select_sql_;
  common::ObString file_path_;
  FileScanOptions options_;
  common::ObSEArray<common::ObString, 16> column_names_;

  DISALLOW_COPY_AND_ASSIGN(ObCopyToFileStmt);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_FILE_STMT_H_
