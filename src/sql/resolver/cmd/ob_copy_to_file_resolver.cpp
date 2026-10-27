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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/cmd/ob_copy_to_file_resolver.h"
#include "sql/resolver/cmd/ob_copy_to_file_stmt.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_basic_session_info.h"

namespace oceanbase
{
namespace sql
{

using namespace common;

int ObCopyToFileResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObCopyToFileStmt *copy_stmt = NULL;
  if (OB_ISNULL(session_info_) || OB_ISNULL(allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!session_info_->is_file_sql_enabled()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy to file when enable_file_sql is disabled");
  } else if (OB_UNLIKELY(T_COPY_TO_FILE != parse_tree.type_ || parse_tree.num_child_ < 3) ||
             OB_ISNULL(parse_tree.children_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(copy_stmt = create_stmt<ObCopyToFileStmt>())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    const ParseNode *source_node = parse_tree.children_[0];
    const ParseNode *path_node = parse_tree.children_[1];
    const ParseNode *options_node = parse_tree.children_[2];

    if (OB_ISNULL(source_node) || OB_ISNULL(path_node)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // 1. Source SQL
      ObString source_sql;
      if (source_node->str_value_ != NULL && source_node->str_len_ > 0) {
        ObString raw_sql(source_node->str_len_, source_node->str_value_);
        if (OB_FAIL(ob_write_string(*allocator_, raw_sql, source_sql))) {
        }
      } else {
        // Source is relation_factor table name
        ObSqlString sql_buf;
        if (OB_FAIL(sql_buf.append_fmt("SELECT * FROM %.*s",
                                       static_cast<int>(source_node->str_len_),
                                       source_node->str_value_))) {
        } else if (OB_FAIL(ob_write_string(*allocator_, sql_buf.string(), source_sql))) {
        }
      }

      // Extract column names if source is select statement with project list
      if (OB_SUCC(ret) && source_node->num_child_ > PARSE_SELECT_SELECT &&
          NULL != source_node->children_[PARSE_SELECT_SELECT]) {
        const ParseNode *field_list_node = source_node->children_[PARSE_SELECT_SELECT];
        for (int32_t i = 0; OB_SUCC(ret) && i < field_list_node->num_child_; ++i) {
          const ParseNode *project_node = field_list_node->children_[i];
          if (NULL != project_node) {
            ObString col_name;
            const ParseNode *inner = (project_node->num_child_ > 0) ? project_node->children_[0] : NULL;
            if (NULL != inner && T_ALIAS == inner->type_ && inner->num_child_ >= 2 && NULL != inner->children_[1]) {
              ParseNode *alias_node = inner->children_[1];
              col_name.assign_ptr(const_cast<char*>(alias_node->str_value_), static_cast<int32_t>(alias_node->str_len_));
            } else if (project_node->str_value_ != NULL && project_node->str_len_ > 0) {
              col_name.assign_ptr(const_cast<char*>(project_node->str_value_), static_cast<int32_t>(project_node->str_len_));
            }
            if (!col_name.empty()) {
              ObString name_copy;
              if (OB_FAIL(ob_write_string(*allocator_, col_name, name_copy))) {
              } else if (OB_FAIL(copy_stmt->add_column_name(name_copy))) {
              }
            }
          }
        }
      }

      // 2. Target File Path
      ObString target_file;
      if (OB_SUCC(ret)) {
        if (T_VARCHAR == path_node->type_ || T_CHAR == path_node->type_) {
          ObString raw_path(path_node->str_len_, path_node->str_value_);
          if (OB_FAIL(ob_write_string(*allocator_, raw_path, target_file))) {
          }
        } else {
          ret = OB_INVALID_ARGUMENT;
        }
      }

      // 3. Security check
      if (OB_SUCC(ret)) {
        ObString secure_file_priv;
        if (OB_FAIL(session_info_->get_secure_file_priv(secure_file_priv))) {
        } else if (!session_info_->is_inner() && OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, target_file))) {
        }
      }

      // 4. Parse Options
      FileScanOptions options;
      options.has_header_ = false; // default for copy unless header specified
      const ParseNode *cur_opt = options_node;
      while (OB_SUCC(ret) && cur_opt != NULL) {
        const ParseNode *opt_item = NULL;
        if (T_LINK_NODE == cur_opt->type_) {
          opt_item = cur_opt->children_[0];
          cur_opt = cur_opt->children_[1];
        } else {
          opt_item = cur_opt;
          cur_opt = NULL;
        }

        if (OB_NOT_NULL(opt_item) && T_COPY_OPTION == opt_item->type_) {
          if (opt_item->num_child_ >= 1 && OB_NOT_NULL(opt_item->children_[0])) {
            ObString opt_name(opt_item->children_[0]->str_len_, opt_item->children_[0]->str_value_);
            if (0 == opt_name.case_compare("header")) {
              options.has_header_ = true;
            } else if (0 == opt_name.case_compare("format") && opt_item->num_child_ >= 2) {
              const ParseNode *vnode = opt_item->children_[1];
              if (OB_NOT_NULL(vnode)) {
                ObString vstr(vnode->str_len_, vnode->str_value_);
                options.format_ = vstr;
              }
            } else if (0 == opt_name.case_compare("csv")) {
              options.format_ = ObString::make_string("csv");
            } else if (0 == opt_name.case_compare("nullstr") && opt_item->num_child_ >= 2) {
              const ParseNode *vnode = opt_item->children_[1];
              if (OB_NOT_NULL(vnode)) {
                ObString vstr(vnode->str_len_, vnode->str_value_);
                if (OB_FAIL(ob_write_string(*allocator_, vstr, options.nullstr_))) {
                }
              }
            } else if (0 == opt_name.case_compare("delimiter") && opt_item->num_child_ >= 2) {
              const ParseNode *vnode = opt_item->children_[1];
              if (OB_NOT_NULL(vnode) && vnode->str_len_ > 0) {
                options.delimiter_ = vnode->str_value_[0];
              }
            }
          }
        }
      }

      if (OB_SUCC(ret)) {
        copy_stmt->set_select_sql(source_sql);
        copy_stmt->set_file_path(target_file);
        copy_stmt->set_options(options);
        stmt_ = copy_stmt;
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
