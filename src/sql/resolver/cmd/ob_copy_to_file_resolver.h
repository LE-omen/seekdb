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

#ifndef OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_FILE_RESOLVER_H_
#define OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_FILE_RESOLVER_H_

#include "sql/resolver/ob_stmt_resolver.h"

namespace oceanbase
{
namespace sql
{

class ObCopyToFileResolver : public ObStmtResolver
{
public:
  explicit ObCopyToFileResolver(ObResolverParams &params)
    : ObStmtResolver(params)
  {}
  virtual ~ObCopyToFileResolver() {}

  virtual int resolve(const ParseNode &parse_tree);

private:
  DISALLOW_COPY_AND_ASSIGN(ObCopyToFileResolver);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_FILE_RESOLVER_H_
