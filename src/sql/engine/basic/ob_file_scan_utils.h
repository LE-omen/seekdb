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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_

#include "share/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "lib/container/ob_se_array.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace common
{
namespace sqlclient
{
class ObMySQLResult;
}
}

namespace sql
{

struct FileColumnMeta {
  common::ObString col_name_;
  common::ObObjType col_type_;
  bool is_nullable_;

  FileColumnMeta() : col_name_(), col_type_(common::ObMaxType), is_nullable_(false) {}
  FileColumnMeta(const common::ObString &name, common::ObObjType type, bool nullable)
    : col_name_(name), col_type_(type), is_nullable_(nullable) {}
};

struct FileScanOptions {
  bool has_header_;
  int64_t sample_size_;
  common::ObString format_;
  common::ObString nullstr_;
  char delimiter_;

  FileScanOptions()
    : has_header_(true),
      sample_size_(-1),
      format_(),
      nullstr_(),
      delimiter_(',')
  {}
};

class ObFileScanUtils {
public:
  // CSV Scanner
  static int scan_csv_file(
      common::ObIAllocator &allocator,
      const common::ObString &file_path,
      const FileScanOptions &options,
      common::ObIArray<FileColumnMeta> &columns,
      common::ObIArray<common::ObArray<common::ObObj>> &rows);

  // JSON/JSONL Scanner
  static int scan_json_file(
      common::ObIAllocator &allocator,
      const common::ObString &file_path,
      const FileScanOptions &options,
      common::ObIArray<FileColumnMeta> &columns,
      common::ObIArray<common::ObArray<common::ObObj>> &rows);

  // Parquet Scanner
  static int scan_parquet_file(
      common::ObIAllocator &allocator,
      const common::ObString &file_path,
      common::ObIArray<FileColumnMeta> &columns,
      common::ObIArray<common::ObArray<common::ObObj>> &rows);

  // Glob Scanner
  static int scan_glob(
      common::ObIAllocator &allocator,
      const common::ObString &pattern,
      common::ObIArray<FileColumnMeta> &columns,
      common::ObIArray<common::ObArray<common::ObObj>> &rows);

  // Export query result to CSV
  static int export_to_csv(
      const common::ObString &file_path,
      const FileScanOptions &options,
      const common::ObIArray<common::ObString> &column_names,
      common::sqlclient::ObMySQLResult *result);

  // Snappy raw decompressor helper
  static int snappy_decompress(
      const char *compressed,
      int64_t compressed_len,
      char *uncompressed,
      int64_t &uncompressed_len);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
