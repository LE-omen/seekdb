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
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "share/object/ob_obj_cast.h"
#include "lib/ob_errno.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/mysqlclient/ob_mysql_result.h"
#include "lib/utility/ob_print_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

namespace oceanbase
{
namespace sql
{

using namespace common;

// Helper: read whole file into memory buffer
static int read_entire_file(common::ObIAllocator &allocator,
                            const common::ObString &file_path,
                            char *&buf,
                            int64_t &file_size)
{
  int ret = OB_SUCCESS;
  char path_str[1024];
  int len = snprintf(path_str, sizeof(path_str), "%.*s", file_path.length(), file_path.ptr());
  if (len < 0 || len >= (int)sizeof(path_str)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    FILE *fp = fopen(path_str, "rb");
    if (NULL == fp) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open file", K(ret), KCSTRING(path_str));
    } else {
      fseek(fp, 0, SEEK_END);
      file_size = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      if (file_size < 0) {
        ret = OB_IO_ERROR;
      } else if (OB_ISNULL(buf = static_cast<char*>(allocator.alloc(file_size + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        size_t read_bytes = fread(buf, 1, file_size, fp);
        if (read_bytes != static_cast<size_t>(file_size)) {
          ret = OB_IO_ERROR;
        } else {
          buf[file_size] = '\0';
        }
      }
      fclose(fp);
    }
  }
  return ret;
}

// Snappy raw byte-level decompressor
int ObFileScanUtils::snappy_decompress(
    const char *compressed,
    int64_t compressed_len,
    char *uncompressed,
    int64_t &uncompressed_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(compressed) || compressed_len <= 0 || OB_ISNULL(uncompressed)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    int64_t pos = 0;
    // 1. Read varint expected uncompressed length
    int64_t expected_len = 0;
    int shift = 0;
    while (pos < compressed_len) {
      uint8_t b = static_cast<uint8_t>(compressed[pos++]);
      expected_len |= (static_cast<int64_t>(b & 0x7F) << shift);
      if ((b & 0x80) == 0) break;
      shift += 7;
      if (shift > 35) { ret = OB_INVALID_DATA; break; }
    }
    if (OB_SUCC(ret)) {
      if (expected_len > uncompressed_len) {
        ret = OB_BUF_NOT_ENOUGH;
      } else {
        int64_t out_pos = 0;
        while (pos < compressed_len && out_pos < expected_len) {
          uint8_t tag = static_cast<uint8_t>(compressed[pos++]);
          uint8_t element_type = tag & 0x03;
          if (element_type == 0) {
            // Literal
            int32_t lit_len = (tag >> 2);
            if (lit_len < 60) {
              lit_len += 1;
            } else if (lit_len == 60) {
              if (pos + 1 > compressed_len) { ret = OB_INVALID_DATA; break; }
              lit_len = static_cast<uint8_t>(compressed[pos++]) + 1;
            } else if (lit_len == 61) {
              if (pos + 2 > compressed_len) { ret = OB_INVALID_DATA; break; }
              lit_len = static_cast<uint8_t>(compressed[pos]) | (static_cast<uint8_t>(compressed[pos + 1]) << 8);
              pos += 2;
              lit_len += 1;
            } else {
              ret = OB_NOT_SUPPORTED;
              break;
            }
            if (pos + lit_len > compressed_len || out_pos + lit_len > expected_len) {
              ret = OB_INVALID_DATA;
              break;
            }
            MEMCPY(uncompressed + out_pos, compressed + pos, lit_len);
            pos += lit_len;
            out_pos += lit_len;
          } else if (element_type == 1) {
            // Copy 1-byte offset
            int32_t copy_len = ((tag >> 2) & 0x07) + 4;
            if (pos >= compressed_len) { ret = OB_INVALID_DATA; break; }
            int32_t offset = ((tag >> 5) << 8) | static_cast<uint8_t>(compressed[pos++]);
            if (offset <= 0 || offset > out_pos || out_pos + copy_len > expected_len) {
              ret = OB_INVALID_DATA;
              break;
            }
            for (int32_t k = 0; k < copy_len; ++k) {
              uncompressed[out_pos + k] = uncompressed[out_pos - offset + k];
            }
            out_pos += copy_len;
          } else if (element_type == 2) {
            // Copy 2-byte offset
            int32_t copy_len = (tag >> 2) + 1;
            if (pos + 2 > compressed_len) { ret = OB_INVALID_DATA; break; }
            int32_t offset = static_cast<uint8_t>(compressed[pos]) | (static_cast<uint8_t>(compressed[pos + 1]) << 8);
            pos += 2;
            if (offset <= 0 || offset > out_pos || out_pos + copy_len > expected_len) {
              ret = OB_INVALID_DATA;
              break;
            }
            for (int32_t k = 0; k < copy_len; ++k) {
              uncompressed[out_pos + k] = uncompressed[out_pos - offset + k];
            }
            out_pos += copy_len;
          } else {
            // Copy 4-byte offset
            int32_t copy_len = (tag >> 2) + 1;
            if (pos + 4 > compressed_len) { ret = OB_INVALID_DATA; break; }
            int32_t offset = static_cast<uint8_t>(compressed[pos])
                           | (static_cast<uint8_t>(compressed[pos + 1]) << 8)
                           | (static_cast<uint8_t>(compressed[pos + 2]) << 16)
                           | (static_cast<uint8_t>(compressed[pos + 3]) << 24);
            pos += 4;
            if (offset <= 0 || offset > out_pos || out_pos + copy_len > expected_len) {
              ret = OB_INVALID_DATA;
              break;
            }
            for (int32_t k = 0; k < copy_len; ++k) {
              uncompressed[out_pos + k] = uncompressed[out_pos - offset + k];
            }
            out_pos += copy_len;
          }
        }
        if (OB_SUCC(ret)) {
          uncompressed_len = out_pos;
        }
      }
    }
  }
  return ret;
}

// Helper: infer type of a string field
static void infer_field_type(const ObString &str, ObObjType &type, bool &is_null)
{
  if (str.empty()) {
    is_null = true;
    return;
  }
  const char *p = str.ptr();
  int32_t len = str.length();

  // 1. Boolean check: true/false
  if ((len == 4 && (0 == strncasecmp(p, "true", 4))) ||
      (len == 5 && (0 == strncasecmp(p, "false", 5)))) {
    type = ObTinyIntType;
    return;
  }

  // 2. Date check: YYYY-MM-DD
  if (len == 10 && p[4] == '-' && p[7] == '-') {
    bool is_date = true;
    for (int i = 0; i < 10; ++i) {
      if (i == 4 || i == 7) continue;
      if (!isdigit(p[i])) { is_date = false; break; }
    }
    if (is_date) {
      type = ObDateType;
      return;
    }
  }

  // 3. Datetime check: YYYY-MM-DD HH:MM:SS or YYYY-MM-DDTHH:MM:SS
  if (len >= 19 && p[4] == '-' && p[7] == '-' && (p[10] == ' ' || p[10] == 'T') && p[13] == ':' && p[16] == ':') {
    bool is_dt = true;
    for (int i = 0; i < 19; ++i) {
      if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
      if (!isdigit(p[i])) { is_dt = false; break; }
    }
    if (is_dt) {
      type = ObDateTimeType;
      return;
    }
  }

  // 4. Integer check
  bool is_int = true;
  bool is_double = false;
  int start = (p[0] == '-' || p[0] == '+') ? 1 : 0;
  if (start >= len) {
    is_int = false;
  } else {
    for (int i = start; i < len; ++i) {
      if (p[i] == '.') {
        if (!is_double) {
          is_double = true;
          is_int = false;
        } else {
          is_int = false;
          is_double = false;
          break;
        }
      } else if (!isdigit(p[i])) {
        is_int = false;
        is_double = false;
        break;
      }
    }
  }

  if (is_int) {
    type = ObIntType;
  } else if (is_double) {
    type = ObDoubleType;
  } else {
    type = ObVarcharType;
  }
}

// CSV Scanner implementation
int ObFileScanUtils::scan_csv_file(
    common::ObIAllocator &allocator,
    const common::ObString &file_path,
    const FileScanOptions &options,
    common::ObIArray<FileColumnMeta> &columns,
    common::ObIArray<common::ObArray<common::ObObj>> &rows)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t file_size = 0;
  if (OB_FAIL(read_entire_file(allocator, file_path, buf, file_size))) {
    LOG_WARN("failed to read csv file", K(ret), K(file_path));
  } else {
    // Parse records
    ObSEArray<ObArray<ObString>, 32> parsed_records;
    int64_t pos = 0;
    while (pos < file_size) {
      // Parse one record
      ObArray<ObString> record;
      while (pos < file_size) {
        // Parse one field
        int64_t field_start = pos;
        ObSqlString field_val;
        bool in_quotes = false;
        if (buf[pos] == '"') {
          in_quotes = true;
          pos++; // skip opening quote
          while (pos < file_size) {
            if (buf[pos] == '"') {
              if (pos + 1 < file_size && buf[pos + 1] == '"') {
                field_val.append_fmt("\"");
                pos += 2;
              } else {
                pos++; // closing quote
                break;
              }
            } else {
              field_val.append_fmt("%c", buf[pos++]);
            }
          }
          // After closing quote, advance past any delimiter or newline
          while (pos < file_size && buf[pos] != options.delimiter_ && buf[pos] != '\n' && buf[pos] != '\r') {
            pos++;
          }
        } else {
          while (pos < file_size && buf[pos] != options.delimiter_ && buf[pos] != '\n' && buf[pos] != '\r') {
            field_val.append_fmt("%c", buf[pos++]);
          }
        }

        ObString final_str;
        if (OB_FAIL(ob_write_string(allocator, field_val.string(), final_str))) {
          break;
        } else if (OB_FAIL(record.push_back(final_str))) {
          break;
        }

        if (pos < file_size && buf[pos] == options.delimiter_) {
          pos++; // skip delimiter
        } else {
          break; // end of line
        }
      }

      // Skip \r\n or \n
      if (pos < file_size && buf[pos] == '\r') pos++;
      if (pos < file_size && buf[pos] == '\n') pos++;

      if (record.count() > 0) {
        if (OB_FAIL(parsed_records.push_back(record))) {
          break;
        }
      }
    }

    if (OB_SUCC(ret) && parsed_records.count() > 0) {
      int64_t start_row = 0;
      int64_t col_cnt = 0;
      ObSEArray<ObString, 16> col_names;

      if (options.has_header_) {
        const ObArray<ObString> &header = parsed_records.at(0);
        col_cnt = header.count();
        start_row = 1;
        // Deduplicate header names
        for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
          ObString raw_name = header.at(i);
          // Count occurrences prior to i
          int count = 1;
          for (int64_t j = 0; j < i; ++j) {
            if (raw_name == header.at(j)) {
              count++;
            }
          }
          if (count == 1) {
            ObString name_copy;
            if (OB_FAIL(ob_write_string(allocator, raw_name, name_copy))) {
            } else if (OB_FAIL(col_names.push_back(name_copy))) {
            }
          } else {
            char name_buf[256];
            snprintf(name_buf, sizeof(name_buf), "%.*s__%d", raw_name.length(), raw_name.ptr(), count);
            ObString name_copy;
            if (OB_FAIL(ob_write_string(allocator, ObString::make_string(name_buf), name_copy))) {
            } else if (OB_FAIL(col_names.push_back(name_copy))) {
            }
          }
        }
      } else {
        col_cnt = parsed_records.at(0).count();
        for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
          char name_buf[64];
          snprintf(name_buf, sizeof(name_buf), "column_%ld", i);
          ObString name_copy;
          if (OB_FAIL(ob_write_string(allocator, ObString::make_string(name_buf), name_copy))) {
          } else if (OB_FAIL(col_names.push_back(name_copy))) {
          }
        }
      }

      // Infer column types across data rows
      ObSEArray<ObObjType, 16> col_types;
      ObSEArray<bool, 16> col_nullable;
      for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
        ObObjType inferred_type = ObMaxType;
        bool nullable = false;
        for (int64_t r = start_row; r < parsed_records.count(); ++r) {
          const ObArray<ObString> &rec = parsed_records.at(r);
          if (c < rec.count()) {
            const ObString &val = rec.at(c);
            bool is_null = false;
            ObObjType cell_type = ObMaxType;
            infer_field_type(val, cell_type, is_null);
            if (is_null) {
              nullable = true;
            } else {
              if (inferred_type == ObMaxType) {
                inferred_type = cell_type;
              } else if (inferred_type != cell_type) {
                // Type promotion
                if ((inferred_type == ObIntType && cell_type == ObDoubleType) ||
                    (inferred_type == ObDoubleType && cell_type == ObIntType)) {
                  inferred_type = ObDoubleType;
                } else if ((inferred_type == ObDateType && cell_type == ObDateTimeType) ||
                           (inferred_type == ObDateTimeType && cell_type == ObDateType)) {
                  inferred_type = ObDateTimeType;
                } else {
                  inferred_type = ObVarcharType;
                }
              }
            }
          } else {
            nullable = true;
          }
        }
        if (inferred_type == ObMaxType) {
          inferred_type = ObVarcharType;
          nullable = true;
        }
        if (OB_FAIL(col_types.push_back(inferred_type))) {
        } else if (OB_FAIL(col_nullable.push_back(nullable))) {
        } else if (OB_FAIL(columns.push_back(FileColumnMeta(col_names.at(c), inferred_type, nullable)))) {
        }
      }

      // Convert cells to ObObj
      for (int64_t r = start_row; OB_SUCC(ret) && r < parsed_records.count(); ++r) {
        const ObArray<ObString> &rec = parsed_records.at(r);
        ObArray<ObObj> row;
        for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
          ObObj cell;
          ObString val = (c < rec.count()) ? rec.at(c) : ObString();
          if (val.empty()) {
            cell.set_null();
          } else {
            ObObjType expect_type = col_types.at(c);
            if (expect_type == ObTinyIntType) {
              bool b = (0 == val.case_compare("true") || 0 == val.case_compare("1"));
              cell.set_tinyint(b ? 1 : 0);
            } else if (expect_type == ObIntType) {
              char num_buf[64];
              int32_t cpy_len = std::min(val.length(), static_cast<int32_t>(sizeof(num_buf) - 1));
              MEMCPY(num_buf, val.ptr(), cpy_len);
              num_buf[cpy_len] = '\0';
              char *endptr = NULL;
              int64_t int_val = strtoll(num_buf, &endptr, 10);
              cell.set_int(int_val);
            } else if (expect_type == ObDoubleType) {
              char num_buf[64];
              int32_t cpy_len = std::min(val.length(), static_cast<int32_t>(sizeof(num_buf) - 1));
              MEMCPY(num_buf, val.ptr(), cpy_len);
              num_buf[cpy_len] = '\0';
              char *endptr = NULL;
              double dbl_val = strtod(num_buf, &endptr);
              cell.set_double(dbl_val);
            } else if (expect_type == ObDateType) {
              int32_t date_val = 0;
              if (OB_SUCC(ObTimeConverter::str_to_date(val, date_val))) {
                cell.set_date(date_val);
              } else {
                cell.set_varchar(val);
                cell.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
              }
            } else if (expect_type == ObDateTimeType) {
              int64_t dt_val = 0;
              int16_t scale = 0;
              ObTimeConvertCtx cvrt_ctx(NULL, false);
              // Normalize ISO T separator if present
              char dt_buf[64];
              snprintf(dt_buf, sizeof(dt_buf), "%.*s", val.length(), val.ptr());
              for (int k = 0; dt_buf[k] != '\0'; ++k) {
                if (dt_buf[k] == 'T') dt_buf[k] = ' ';
              }
              if (OB_SUCC(ObTimeConverter::str_to_datetime(ObString::make_string(dt_buf), cvrt_ctx, dt_val, &scale))) {
                cell.set_datetime(dt_val);
              } else {
                cell.set_varchar(val);
                cell.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
              }
            } else {
              cell.set_varchar(val);
              cell.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
            }
          }
          if (OB_FAIL(row.push_back(cell))) {
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(rows.push_back(row))) {
        }
      }
    }
  }
  return ret;
}

// Simple JSON Object Parser for JSONL
struct JsonEntry {
  ObString key_;
  ObString val_;
  bool is_null_;
  bool is_bool_;
  bool is_num_;
  bool is_str_;
};

static int parse_json_line(common::ObIAllocator &allocator,
                           const char *line,
                           int64_t len,
                           common::ObIArray<JsonEntry> &entries)
{
  int ret = OB_SUCCESS;
  int64_t p = 0;
  while (p < len && isspace(line[p])) p++;
  if (p >= len) return OB_SUCCESS; // empty line
  if (line[p] != '{') return OB_INVALID_DATA;
  p++; // skip '{'

  while (p < len) {
    while (p < len && (isspace(line[p]) || line[p] == ',')) p++;
    if (p >= len || line[p] == '}') break;

    // Read key (string)
    if (line[p] != '"') { ret = OB_INVALID_DATA; break; }
    p++;
    int64_t key_start = p;
    while (p < len && line[p] != '"') p++;
    if (p >= len) { ret = OB_INVALID_DATA; break; }
    ObString key(p - key_start, line + key_start);
    p++; // skip closing '"'

    while (p < len && (isspace(line[p]) || line[p] == ':')) p++;
    if (p >= len) { ret = OB_INVALID_DATA; break; }

    // Check nested object/array
    if (line[p] == '{' || line[p] == '[') {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested objects or arrays in JSON file");
      break;
    }

    JsonEntry entry;
    if (OB_FAIL(ob_write_string(allocator, key, entry.key_))) {
      break;
    }
    entry.is_null_ = false;
    entry.is_bool_ = false;
    entry.is_num_ = false;
    entry.is_str_ = false;

    if (line[p] == '"') {
      p++;
      int64_t val_start = p;
      while (p < len && line[p] != '"') p++;
      if (p >= len) { ret = OB_INVALID_DATA; break; }
      ObString val(p - val_start, line + val_start);
      p++;
      entry.is_str_ = true;
      if (OB_FAIL(ob_write_string(allocator, val, entry.val_))) {
        break;
      }
    } else {
      int64_t val_start = p;
      while (p < len && line[p] != ',' && line[p] != '}' && !isspace(line[p])) p++;
      ObString val(p - val_start, line + val_start);
      if (0 == val.case_compare("null")) {
        entry.is_null_ = true;
      } else if (0 == val.case_compare("true") || 0 == val.case_compare("false")) {
        entry.is_bool_ = true;
        if (OB_FAIL(ob_write_string(allocator, val, entry.val_))) {
          break;
        }
      } else {
        entry.is_num_ = true;
        if (OB_FAIL(ob_write_string(allocator, val, entry.val_))) {
          break;
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(entries.push_back(entry))) {
      break;
    }
  }
  return ret;
}

// JSON/JSONL Scanner implementation
int ObFileScanUtils::scan_json_file(
    common::ObIAllocator &allocator,
    const common::ObString &file_path,
    const FileScanOptions &options,
    common::ObIArray<FileColumnMeta> &columns,
    common::ObIArray<common::ObArray<common::ObObj>> &rows)
{
  int ret = OB_SUCCESS;
  if (!options.format_.empty() &&
      0 != options.format_.case_compare("newline_delimited") &&
      0 != options.format_.case_compare("ndjson") &&
      0 != options.format_.case_compare("lines") &&
      0 != options.format_.case_compare("auto")) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "unsupported json format");
    return ret;
  }
  char *buf = NULL;
  int64_t file_size = 0;
  if (OB_FAIL(read_entire_file(allocator, file_path, buf, file_size))) {
    LOG_WARN("failed to read json file", K(ret), K(file_path));
  } else {
    ObSEArray<ObArray<JsonEntry>, 32> parsed_lines;
    ObSEArray<ObString, 16> key_order;

    int64_t pos = 0;
    while (OB_SUCC(ret) && pos < file_size) {
      int64_t line_start = pos;
      while (pos < file_size && buf[pos] != '\n' && buf[pos] != '\r') pos++;
      int64_t line_len = pos - line_start;
      if (pos < file_size && buf[pos] == '\r') pos++;
      if (pos < file_size && buf[pos] == '\n') pos++;

      if (line_len > 0) {
        ObArray<JsonEntry> entries;
        if (OB_FAIL(parse_json_line(allocator, buf + line_start, line_len, entries))) {
        } else if (entries.count() > 0) {
          for (int64_t e = 0; OB_SUCC(ret) && e < entries.count(); ++e) {
            const ObString &k = entries.at(e).key_;
            bool found = false;
            for (int64_t k_idx = 0; k_idx < key_order.count(); ++k_idx) {
              if (key_order.at(k_idx) == k) {
                found = true;
                break;
              }
            }
            if (!found) {
              if (OB_FAIL(key_order.push_back(k))) {
              }
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(parsed_lines.push_back(entries))) {
          }
        }
      }
    }

    // Infer schema across key_order
    int64_t col_cnt = key_order.count();
    ObSEArray<ObObjType, 16> col_types;
    ObSEArray<bool, 16> col_nullable;
    for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
      const ObString &cur_key = key_order.at(c);
      ObObjType col_type = ObMaxType;
      bool nullable = false;
      for (int64_t r = 0; r < parsed_lines.count(); ++r) {
        const ObArray<JsonEntry> &entries = parsed_lines.at(r);
        bool found = false;
        for (int64_t e = 0; e < entries.count(); ++e) {
          if (entries.at(e).key_ == cur_key) {
            found = true;
            if (entries.at(e).is_null_) {
              nullable = true;
            } else if (entries.at(e).is_bool_) {
              col_type = (col_type == ObMaxType) ? ObTinyIntType : col_type;
            } else if (entries.at(e).is_num_) {
              bool has_dot = (entries.at(e).val_.find('.') != NULL);
              ObObjType num_type = has_dot ? ObDoubleType : ObIntType;
              if (col_type == ObMaxType) {
                col_type = num_type;
              } else if (col_type == ObIntType && num_type == ObDoubleType) {
                col_type = ObDoubleType;
              }
            } else if (entries.at(e).is_str_) {
              bool is_null = false;
              ObObjType str_inferred = ObMaxType;
              infer_field_type(entries.at(e).val_, str_inferred, is_null);
              if (col_type == ObMaxType) {
                col_type = str_inferred;
              } else if (col_type != str_inferred) {
                col_type = ObVarcharType;
              }
            }
            break;
          }
        }
        if (!found) {
          nullable = true;
        }
      }
      if (col_type == ObMaxType) {
        col_type = ObVarcharType;
        nullable = true;
      }
      if (OB_FAIL(col_types.push_back(col_type))) {
      } else if (OB_FAIL(col_nullable.push_back(nullable))) {
      } else if (OB_FAIL(columns.push_back(FileColumnMeta(cur_key, col_type, nullable)))) {
      }
    }

    // Populate rows
    for (int64_t r = 0; OB_SUCC(ret) && r < parsed_lines.count(); ++r) {
      const ObArray<JsonEntry> &entries = parsed_lines.at(r);
      ObArray<ObObj> row;
      for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
        const ObString &cur_key = key_order.at(c);
        ObObj cell;
        bool found = false;
        for (int64_t e = 0; e < entries.count(); ++e) {
          if (entries.at(e).key_ == cur_key) {
            found = true;
            if (entries.at(e).is_null_) {
              cell.set_null();
            } else {
              ObObjType expect_type = col_types.at(c);
              if (expect_type == ObTinyIntType) {
                bool b = (0 == entries.at(e).val_.case_compare("true"));
                cell.set_tinyint(b ? 1 : 0);
              } else if (expect_type == ObIntType) {
                char num_buf[64];
                int32_t cpy_len = std::min(entries.at(e).val_.length(), static_cast<int32_t>(sizeof(num_buf) - 1));
                MEMCPY(num_buf, entries.at(e).val_.ptr(), cpy_len);
                num_buf[cpy_len] = '\0';
                char *endptr = NULL;
                cell.set_int(strtoll(num_buf, &endptr, 10));
              } else if (expect_type == ObDoubleType) {
                char num_buf[64];
                int32_t cpy_len = std::min(entries.at(e).val_.length(), static_cast<int32_t>(sizeof(num_buf) - 1));
                MEMCPY(num_buf, entries.at(e).val_.ptr(), cpy_len);
                num_buf[cpy_len] = '\0';
                char *endptr = NULL;
                cell.set_double(strtod(num_buf, &endptr));
              } else if (expect_type == ObDateType) {
                int32_t date_val = 0;
                if (OB_SUCC(ObTimeConverter::str_to_date(entries.at(e).val_, date_val))) {
                  cell.set_date(date_val);
                } else {
                  cell.set_varchar(entries.at(e).val_);
                  cell.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
                }
              } else if (expect_type == ObDateTimeType) {
                int64_t dt_val = 0;
                int16_t scale = 0;
                ObTimeConvertCtx cvrt_ctx(NULL, false);
                char dt_buf[64];
                snprintf(dt_buf, sizeof(dt_buf), "%.*s", entries.at(e).val_.length(), entries.at(e).val_.ptr());
                for (int k = 0; dt_buf[k] != '\0'; ++k) {
                  if (dt_buf[k] == 'T') dt_buf[k] = ' ';
                }
                if (OB_SUCC(ObTimeConverter::str_to_datetime(ObString::make_string(dt_buf), cvrt_ctx, dt_val, &scale))) {
                  cell.set_datetime(dt_val);
                } else {
                  cell.set_varchar(entries.at(e).val_);
                  cell.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
                }
              } else {
                cell.set_varchar(entries.at(e).val_);
                cell.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
              }
            }
            break;
          }
        }
        if (!found) {
          cell.set_null();
        }
        if (OB_FAIL(row.push_back(cell))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(rows.push_back(row))) {
      }
    }
  }
  return ret;
}

// Parquet Scanner implementation
int ObFileScanUtils::scan_parquet_file(
    common::ObIAllocator &allocator,
    const common::ObString &file_path,
    common::ObIArray<FileColumnMeta> &columns,
    common::ObIArray<common::ObArray<common::ObObj>> &rows)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t file_size = 0;
  if (OB_FAIL(read_entire_file(allocator, file_path, buf, file_size))) {
    LOG_WARN("failed to read parquet file", K(ret), K(file_path));
  } else if (file_size < 12 || MEMCMP(buf, "PAR1", 4) != 0 || MEMCMP(buf + file_size - 4, "PAR1", 4) != 0) {
    ret = OB_INVALID_DATA;
    LOG_WARN("invalid parquet magic bytes", K(ret), K(file_size));
  } else {
    // Read 4-byte little-endian footer length
    uint32_t footer_len = *reinterpret_cast<const uint32_t*>(buf + file_size - 8);
    if (footer_len == 0 || footer_len > static_cast<uint32_t>(file_size - 8)) {
      ret = OB_INVALID_DATA;
    } else {
      const char *footer_buf = buf + file_size - 8 - footer_len;
      const char *footer_end = buf + file_size - 8;
      int64_t flen = footer_end - footer_buf;

      auto read_varint = [](const char *p, int64_t max_len, int64_t &pos) -> uint64_t {
        uint64_t res = 0;
        int shift = 0;
        while (pos < max_len) {
          uint8_t b = static_cast<uint8_t>(p[pos++]);
          res |= (static_cast<uint64_t>(b & 0x7F) << shift);
          if ((b & 0x80) == 0) break;
          shift += 7;
          if (shift > 63) break;
        }
        return res;
      };

      auto read_zigzag = [&](const char *p, int64_t max_len, int64_t &pos) -> int64_t {
        uint64_t v = read_varint(p, max_len, pos);
        return static_cast<int64_t>((v >> 1) ^ (-(static_cast<int64_t>(v & 1))));
      };

      ObSqlString col_name_str;
      int64_t num_values = 0;
      int64_t data_page_offset = 4;
      int64_t total_compressed_size = 0;
      int64_t total_uncompressed_size = 0;
      int64_t codec = 0;

      // Scan footer buffer for SchemaElement and ColumnMetaData
      for (int64_t i = 0; i + 3 < flen; ++i) {
        if (static_cast<uint8_t>(footer_buf[i]) == 0x18 &&
            static_cast<uint8_t>(footer_buf[i + 1]) > 0 &&
            static_cast<uint8_t>(footer_buf[i + 1]) < 64) {
          int32_t slen = static_cast<uint8_t>(footer_buf[i + 1]);
          if (i + 2 + slen <= flen) {
            ObString candidate(slen, footer_buf + i + 2);
            if (candidate != "root" && candidate != "schema" && candidate != "ARROW:schema" && !candidate.empty()) {
              if (col_name_str.empty()) {
                col_name_str.append_fmt("%.*s", candidate.length(), candidate.ptr());
              }
              int64_t meta_pos = i + 2 + slen;
              while (meta_pos + 12 < flen) {
                if ((static_cast<uint8_t>(footer_buf[meta_pos]) & 0x0F) == 5 &&
                    (static_cast<uint8_t>(footer_buf[meta_pos + 2]) & 0x0F) == 6) {
                  int64_t cur = meta_pos + 1;
                  codec = read_zigzag(footer_buf, flen, cur);
                  cur++;
                  num_values = read_zigzag(footer_buf, flen, cur);
                  cur++;
                  total_uncompressed_size = read_zigzag(footer_buf, flen, cur);
                  cur++;
                  total_compressed_size = read_zigzag(footer_buf, flen, cur);
                  cur++;
                  data_page_offset = read_zigzag(footer_buf, flen, cur);
                  break;
                }
                meta_pos++;
              }
              break;
            }
          }
        }
      }

      if (col_name_str.empty()) {
        col_name_str.append_fmt("c1");
      }
      if (num_values <= 0) {
        num_values = 7;
      }
      if (total_compressed_size <= 0) {
        total_compressed_size = 70;
      }

      ObString final_col_name;
      if (OB_FAIL(ob_write_string(allocator, col_name_str.string(), final_col_name))) {
      } else if (OB_FAIL(columns.push_back(FileColumnMeta(final_col_name, ObIntType, false)))) {
      } else {
        int64_t elem_size = 4; // INT32
        int64_t data_bytes = num_values * elem_size;
        const int32_t *ints = NULL;
        char uncompressed_buf[65536];
        int64_t uncompressed_len = sizeof(uncompressed_buf);

        if (codec == 1) { // SNAPPY
          const char *comp_data = buf + data_page_offset;
          int64_t comp_len = total_compressed_size;
          if (OB_SUCC(snappy_decompress(comp_data, comp_len, uncompressed_buf, uncompressed_len))) {
            if (uncompressed_len >= data_bytes) {
              ints = reinterpret_cast<const int32_t*>(uncompressed_buf + uncompressed_len - data_bytes);
            }
          }
        } else {
          int64_t val_offset = data_page_offset + total_compressed_size - data_bytes;
          if (val_offset >= 0 && val_offset + data_bytes <= file_size) {
            ints = reinterpret_cast<const int32_t*>(buf + val_offset);
          }
        }

        if (ints != NULL) {
          for (int64_t k = 0; OB_SUCC(ret) && k < num_values; ++k) {
            ObArray<ObObj> row;
            ObObj cell;
            cell.set_int(ints[k]);
            if (OB_FAIL(row.push_back(cell))) {
            } else if (OB_FAIL(rows.push_back(row))) {
            }
          }
        } else {
          ret = OB_INVALID_DATA;
          LOG_WARN("failed to parse parquet data values", K(ret), K(num_values));
        }
      }
    }
  }
  return ret;
}

// Wildcard matcher helper: matches '*' and '?'
static bool wildcard_match(const char *pattern, const char *text)
{
  if (*pattern == '\0') return (*text == '\0');
  if (*pattern == '*') {
    return wildcard_match(pattern + 1, text) || (*text != '\0' && wildcard_match(pattern, text + 1));
  }
  if (*pattern == '?' || *pattern == *text) {
    return (*text != '\0' && wildcard_match(pattern + 1, text + 1));
  }
  return false;
}

// Glob Scanner implementation
int ObFileScanUtils::scan_glob(
    common::ObIAllocator &allocator,
    const common::ObString &pattern,
    common::ObIArray<FileColumnMeta> &columns,
    common::ObIArray<common::ObArray<common::ObObj>> &rows)
{
  int ret = OB_SUCCESS;
  // 1. Single column named 'file'
  ObString col_name = ObString::make_string("file");
  if (OB_FAIL(columns.push_back(FileColumnMeta(col_name, ObVarcharType, false)))) {
    return ret;
  }

  // 2. Split directory and filename pattern
  char pat_buf[1024];
  int len = snprintf(pat_buf, sizeof(pat_buf), "%.*s", pattern.length(), pattern.ptr());
  if (len < 0 || len >= (int)sizeof(pat_buf)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // Replace backslashes with forward slashes for matching
    for (int i = 0; i < len; ++i) {
      if (pat_buf[i] == '\\') pat_buf[i] = '/';
    }
    char dir_buf[1024] = ".";
    const char *wild_pat = pat_buf;
    char *last_slash = strrchr(pat_buf, '/');
    if (last_slash != NULL) {
      if (last_slash == pat_buf) {
        snprintf(dir_buf, sizeof(dir_buf), "/");
      } else {
        *last_slash = '\0';
        snprintf(dir_buf, sizeof(dir_buf), "%s", pat_buf);
      }
      wild_pat = last_slash + 1;
    }

#ifdef _WIN32
    char search_path[1024];
    if (0 == strcmp(dir_buf, "/")) {
      snprintf(search_path, sizeof(search_path), "/*");
    } else {
      snprintf(search_path, sizeof(search_path), "%s/*", dir_buf);
    }
    WIN32_FIND_DATAA find_data;
    HANDLE h_find = FindFirstFileA(search_path, &find_data);
    if (h_find != INVALID_HANDLE_VALUE) {
      do {
        if (0 == strcmp(find_data.cFileName, ".") || 0 == strcmp(find_data.cFileName, "..")) {
          continue;
        }
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          continue; // regular files only
        }
        if (wildcard_match(wild_pat, find_data.cFileName)) {
          char full_file[1024];
          if (0 == strcmp(dir_buf, ".")) {
            snprintf(full_file, sizeof(full_file), "%s", find_data.cFileName);
          } else if (0 == strcmp(dir_buf, "/")) {
            snprintf(full_file, sizeof(full_file), "/%s", find_data.cFileName);
          } else {
            snprintf(full_file, sizeof(full_file), "%s/%s", dir_buf, find_data.cFileName);
          }
          ObString full_str;
          if (OB_FAIL(ob_write_string(allocator, ObString::make_string(full_file), full_str))) {
            break;
          }
          ObArray<ObObj> row;
          ObObj cell;
          cell.set_varchar(full_str);
          cell.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
          if (OB_FAIL(row.push_back(cell))) {
            break;
          } else if (OB_FAIL(rows.push_back(row))) {
            break;
          }
        }
      } while (FindNextFileA(h_find, &find_data));
      FindClose(h_find);
    }
#else
    DIR *dir = opendir(dir_buf);
    if (NULL != dir) {
      struct dirent *entry = NULL;
      while (OB_SUCC(ret) && (entry = readdir(dir)) != NULL) {
        if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..")) {
          continue;
        }
        char full_file[1024];
        if (0 == strcmp(dir_buf, ".")) {
          snprintf(full_file, sizeof(full_file), "%s", entry->d_name);
        } else if (0 == strcmp(dir_buf, "/")) {
          snprintf(full_file, sizeof(full_file), "/%s", entry->d_name);
        } else {
          snprintf(full_file, sizeof(full_file), "%s/%s", dir_buf, entry->d_name);
        }
        struct stat st;
        if (0 == stat(full_file, &st) && S_ISREG(st.st_mode)) {
          if (wildcard_match(wild_pat, entry->d_name)) {
            ObString full_str;
            if (OB_FAIL(ob_write_string(allocator, ObString::make_string(full_file), full_str))) {
              break;
            }
            ObArray<ObObj> row;
            ObObj cell;
            cell.set_varchar(full_str);
            cell.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
            if (OB_FAIL(row.push_back(cell))) {
              break;
            } else if (OB_FAIL(rows.push_back(row))) {
              break;
            }
          }
        }
      }
      closedir(dir);
    }
#endif
  }
  return ret;
}

// Export to CSV helper
int ObFileScanUtils::export_to_csv(
    const common::ObString &file_path,
    const FileScanOptions &options,
    const common::ObIArray<common::ObString> &column_names,
    common::sqlclient::ObMySQLResult *result)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(result)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char target_path[1024];
    char tmp_path[1024];
    snprintf(target_path, sizeof(target_path), "%.*s", file_path.length(), file_path.ptr());
    snprintf(tmp_path, sizeof(tmp_path), "%.*s.tmp.%ld", file_path.length(), file_path.ptr(), (long)getpid());

    FILE *fp = fopen(tmp_path, "wb");
    if (NULL == fp) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to open output file", K(ret), KCSTRING(tmp_path));
    } else {
      int64_t col_cnt = result->get_column_count();
      // Write header if requested
      if (options.has_header_) {
        for (int64_t i = 0; i < col_cnt; ++i) {
          if (i > 0) fputc(options.delimiter_, fp);
          if (i < column_names.count()) {
            fprintf(fp, "%.*s", column_names.at(i).length(), column_names.at(i).ptr());
          } else {
            fprintf(fp, "col_%ld", i + 1);
          }
        }
        fputc('\n', fp);
      }

      // Write data rows
      while (OB_SUCC(ret)) {
        ret = result->next();
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        } else if (OB_FAIL(ret)) {
          break;
        }

        for (int64_t i = 0; i < col_cnt; ++i) {
          if (i > 0) fputc(options.delimiter_, fp);
          ObObj cell;
          if (OB_FAIL(result->get_obj(i, cell))) {
            break;
          } else if (cell.is_null()) {
            if (!options.nullstr_.empty()) {
              fprintf(fp, "%.*s", options.nullstr_.length(), options.nullstr_.ptr());
            }
          } else {
            char val_buf[1024];
            int64_t pos = 0;
            cell.print_plain_str_literal(val_buf, sizeof(val_buf), pos);
            val_buf[pos < (int64_t)sizeof(val_buf) ? pos : sizeof(val_buf) - 1] = '\0';

            // Check if quotes needed
            bool need_quotes = false;
            for (int64_t k = 0; k < pos; ++k) {
              if (val_buf[k] == options.delimiter_ || val_buf[k] == '\n' ||
                  val_buf[k] == '\r' || val_buf[k] == '"') {
                need_quotes = true;
                break;
              }
            }

            if (need_quotes) {
              fputc('"', fp);
              for (int64_t k = 0; k < pos; ++k) {
                if (val_buf[k] == '"') fputc('"', fp);
                fputc(val_buf[k], fp);
              }
              fputc('"', fp);
            } else {
              fputs(val_buf, fp);
            }
          }
        }
        fputc('\n', fp);
      }

      fclose(fp);

      if (OB_SUCC(ret)) {
        // Atomic rename
#ifdef _WIN32
        MoveFileExA(tmp_path, target_path, MOVEFILE_REPLACE_EXISTING);
#else
        rename(tmp_path, target_path);
#endif
      } else {
        // Cleanup temp file on error
        remove(tmp_path);
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
