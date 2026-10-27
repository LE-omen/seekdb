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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

OB_INLINE static bool is_fast_builtin_stopword(const char *str, int64_t len)
{
  if (len <= 0 || len > 5 || str == nullptr) {
    return false;
  }
  for (int64_t i = 0; i < len; ++i) {
    if (str[i] < 'a' || str[i] > 'z') {
      return false;
    }
  }
  switch (len) {
    case 1:
      return (str[0] == 'a' || str[0] == 'i');
    case 2:
      switch (str[0]) {
        case 'a': return str[1] == 'n' || str[1] == 's' || str[1] == 't';
        case 'b': return str[1] == 'e' || str[1] == 'y';
        case 'd': return str[1] == 'e';
        case 'e': return str[1] == 'n';
        case 'i': return str[1] == 'n' || str[1] == 's' || str[1] == 't';
        case 'l': return str[1] == 'a';
        case 'o': return str[1] == 'f' || str[1] == 'n' || str[1] == 'r';
        case 't': return str[1] == 'o';
        default: return false;
      }
    case 3:
      switch (str[0]) {
        case 'a': return str[1] == 'r' && str[2] == 'e';
        case 'c': return str[1] == 'o' && str[2] == 'm';
        case 'f': return str[1] == 'o' && str[2] == 'r';
        case 'h': return str[1] == 'o' && str[2] == 'w';
        case 't': return str[1] == 'h' && str[2] == 'e';
        case 'u': return str[1] == 'n' && str[2] == 'd';
        case 'w':
          return (str[1] == 'a' && str[2] == 's') ||
                 (str[1] == 'h' && str[2] == 'o') ||
                 (str[1] == 'w' && str[2] == 'w');
        default: return false;
      }
    case 4:
      switch (str[0]) {
        case 'f': return str[1] == 'r' && str[2] == 'o' && str[3] == 'm';
        case 't':
          return (str[1] == 'h' && str[2] == 'a' && str[3] == 't') ||
                 (str[1] == 'h' && str[2] == 'i' && str[3] == 's');
        case 'w':
          return (str[1] == 'h' && str[2] == 'a' && str[3] == 't') ||
                 (str[1] == 'h' && str[2] == 'e' && str[3] == 'n') ||
                 (str[1] == 'i' && str[2] == 'l' && str[3] == 'l') ||
                 (str[1] == 'i' && str[2] == 't' && str[3] == 'h');
        default: return false;
      }
    case 5:
      if (str[0] == 'a') {
        return str[1] == 'b' && str[2] == 'o' && str[3] == 'u' && str[4] == 't';
      } else if (str[0] == 'w') {
        return str[1] == 'h' && str[2] == 'e' && str[3] == 'r' && str[4] == 'e';
      }
      return false;
    default:
      return false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const common::ObString &word_str = word.get_word().get_string();
    const int32_t len = word_str.length();
    const char *ptr = word_str.ptr();
    if (is_fast_builtin_stopword(ptr, len)) {
      is_stopword = true;
      return OB_SUCCESS;
    }
    if (len > 5 || len == 0) {
      is_stopword = false;
      return OB_SUCCESS;
    }
    bool has_non_ascii = false;
    for (int32_t i = 0; i < len; ++i) {
      if (static_cast<uint8_t>(ptr[i]) >= 0x80) {
        has_non_ascii = true;
        break;
      }
    }
    if (has_non_ascii) {
      is_stopword = false;
      return OB_SUCCESS;
    }

    common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
    common::ObString cmp_str;
    if (OB_FAIL(common::ObCharset::charset_convert(
                                       allocator,
                                       word.get_word().get_string(),
                                       word.get_collation_type(),
                                       stopword_type_.get_collation_type(),
                                       cmp_str))) {
    } else {
      ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
      ret = stopword_set_.exist_refactored(converted);
      if (OB_HASH_NOT_EXIST == ret) {
        is_stopword = false;
        ret = OB_SUCCESS;
      } else if (OB_HASH_EXIST == ret) {
        is_stopword = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    const common::ObString &src_str = src.get_word().get_string();
    const char *ptr = src_str.ptr();
    const int32_t len = src_str.length();
    bool has_upper = false;
    for (int32_t i = 0; i < len; ++i) {
      if (ptr[i] >= 'A' && ptr[i] <= 'Z') {
        has_upper = true;
        break;
      }
    }
    if (!has_upper) {
      dst = src;
    } else {
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(
                      word_meta_.get_collation_type(),
                      src_str,
                      dst_str,
                      allocator_))) {
      } else {
        ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
        dst = tmp;
      }
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  int64_t word_count = 0;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else if (OB_FAIL(word_map_.get_refactored(word, word_count)) && OB_HASH_NOT_EXIST != ret) {
  } else {
    if (OB_HASH_NOT_EXIST == ret) {
      word_count = 1;
    } else {
      word_count += word_freq;
    }
    if (OB_FAIL(word_map_.set_refactored(word, word_count, 1/*overwrite*/))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
