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

#define USING_LOG_PREFIX SQL

#include <cctype>
#include <string>
#include <vector>

#include "ob_sql_parser.h"
#include "sql/parser/parse_node.h"
#include "share/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"


namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{

bool is_ident_boundary(const char c)
{
  return !std::isalnum(static_cast<unsigned char>(c)) && c != '_';
}

bool match_keyword_ci(const char *str, const int64_t len, const int64_t pos, const char *keyword)
{
  const int64_t keyword_len = static_cast<int64_t>(strlen(keyword));
  if (pos < 0 || pos + keyword_len > len) {
    return false;
  }
  for (int64_t i = 0; i < keyword_len; ++i) {
    if (std::tolower(static_cast<unsigned char>(str[pos + i]))
        != std::tolower(static_cast<unsigned char>(keyword[i]))) {
      return false;
    }
  }
  return (pos + keyword_len == len || is_ident_boundary(str[pos + keyword_len]))
      && (pos == 0 || is_ident_boundary(str[pos - 1]));
}

bool match_function_name(const char *str,
                         const int64_t len,
                         const int64_t pos,
                         const char *name)
{
  int64_t next = pos + static_cast<int64_t>(strlen(name));
  bool matched = match_keyword_ci(str, len, pos, name);
  while (matched && next < len
         && std::isspace(static_cast<unsigned char>(str[next]))) {
    ++next;
  }
  return matched && next < len && '(' == str[next];
}

int64_t skip_space(const char *str, const int64_t len, int64_t pos)
{
  while (pos < len && std::isspace(static_cast<unsigned char>(str[pos]))) {
    ++pos;
  }
  return pos;
}

int64_t find_query_to(const char *str, const int64_t len, const int64_t start)
{
  int64_t depth = 0;
  char quote = '\0';
  int64_t result = -1;
  for (int64_t i = start; i < len && result < 0; ++i) {
    const char c = str[i];
    if (quote != '\0') {
      if (c == quote) {
        if (i + 1 < len && str[i + 1] == quote) {
          ++i;
        } else {
          quote = '\0';
        }
      } else if (c == '\\' && i + 1 < len) {
        ++i;
      }
    } else if (c == '\'' || c == '"' || c == '`') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
    } else if (0 == depth && match_keyword_ci(str, len, i, "to")) {
      result = i;
    }
  }
  return result;
}

int64_t find_matching_paren(const char *str, const int64_t len, const int64_t open_pos)
{
  int64_t depth = 0;
  char quote = '\0';
  for (int64_t i = open_pos; i < len; ++i) {
    const char c = str[i];
    if (quote != '\0') {
      if (c == quote) {
        if (i + 1 < len && str[i + 1] == quote) {
          ++i;
        } else {
          quote = '\0';
        }
      } else if (c == '\\' && i + 1 < len) {
        ++i;
      }
    } else if (c == '\'' || c == '"' || c == '`') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')' && 0 == --depth) {
      return i;
    }
  }
  return -1;
}

int read_sql_string(const char *str,
                    const int64_t len,
                    int64_t &pos,
                    std::string &value,
                    bool &quoted)
{
  int ret = OB_SUCCESS;
  value.clear();
  quoted = false;
  if (pos >= len || (str[pos] != '\'' && str[pos] != '"')) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    const char quote = str[pos++];
    quoted = true;
    bool done = false;
    while (OB_SUCC(ret) && pos < len && !done) {
      const char c = str[pos++];
      if (c == quote) {
        if (pos < len && str[pos] == quote) {
          value.push_back(quote);
          ++pos;
        } else {
          done = true;
        }
      } else if (c == '\\' && pos < len) {
        const char escaped = str[pos++];
        if (escaped == 'n') {
          value.push_back('\n');
        } else if (escaped == 'r') {
          value.push_back('\r');
        } else if (escaped == 't') {
          value.push_back('\t');
        } else {
          value.push_back(escaped);
        }
      } else {
        value.push_back(c);
      }
    }
    if (OB_SUCC(ret) && !done) {
      ret = OB_ERR_PARSE_SQL;
    }
  }
  return ret;
}

std::string sql_string_literal(const std::string &value)
{
  std::string result("'");
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '\'' || c == '\\') {
      result.push_back('\\');
      result.push_back(c);
    } else if (c == '\n') {
      result.append("\\n");
    } else if (c == '\r') {
      result.append("\\r");
    } else if (c == '\t') {
      result.append("\\t");
    } else {
      result.push_back(c);
    }
  }
  result.push_back('\'');
  return result;
}

std::string trim_sql_token(const std::string &value)
{
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

void split_copy_options(const std::string &text, std::vector<std::string> &options)
{
  int64_t depth = 0;
  char quote = '\0';
  size_t begin = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : ',';
    if (quote != '\0') {
      if (c == quote) {
        if (i + 1 < text.size() && text[i + 1] == quote) {
          ++i;
        } else {
          quote = '\0';
        }
      }
    } else if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
    } else if (c == ',' && 0 == depth) {
      options.push_back(trim_sql_token(text.substr(begin, i - begin)));
      begin = i + 1;
    }
  }
}

int parse_copy_string_value(const std::string &value, std::string &result)
{
  int ret = OB_SUCCESS;
  const std::string trimmed = trim_sql_token(value);
  int64_t pos = 0;
  bool quoted = false;
  if (OB_FAIL(read_sql_string(trimmed.data(), static_cast<int64_t>(trimmed.size()),
                              pos, result, quoted))) {
  } else if (!quoted) {
    ret = OB_ERR_PARSE_SQL;
  }
  return ret;
}

int parse_copy_bool_value(const std::string &value, bool &result)
{
  int ret = OB_SUCCESS;
  const std::string trimmed = trim_sql_token(value);
  if (trimmed.empty() || 0 == strcasecmp(trimmed.c_str(), "true")
      || 0 == strcasecmp(trimmed.c_str(), "on")
      || 0 == strcasecmp(trimmed.c_str(), "1")) {
    result = true;
  } else if (0 == strcasecmp(trimmed.c_str(), "false")
             || 0 == strcasecmp(trimmed.c_str(), "off")
             || 0 == strcasecmp(trimmed.c_str(), "0")) {
    result = false;
  } else {
    ret = OB_ERR_PARSE_SQL;
  }
  return ret;
}

int parse_copy_options(const char *str,
                       const int64_t len,
                       int64_t &pos,
                       bool &header,
                       std::string &field_delimiter,
                       std::string &field_quote,
                       std::string &escape,
                       std::string &null_string)
{
  int ret = OB_SUCCESS;
  pos = skip_space(str, len, pos);
  if (pos == len) {
    // Defaults: no header, comma delimiter, double-quote enclosure, empty NULL.
  } else if (str[pos] != '(') {
    ret = OB_ERR_PARSE_SQL;
  } else {
    const int64_t close_pos = find_matching_paren(str, len, pos);
    if (close_pos < 0) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      std::string options_text(str + pos + 1,
                               static_cast<size_t>(close_pos - pos - 1));
      std::vector<std::string> options;
      split_copy_options(options_text, options);
      for (size_t i = 0; OB_SUCC(ret) && i < options.size(); ++i) {
        const std::string &option = options[i];
        if (option.empty()) {
          continue;
        }
        size_t key_end = 0;
        while (key_end < option.size()
               && !std::isspace(static_cast<unsigned char>(option[key_end]))
               && option[key_end] != '=') {
          ++key_end;
        }
        std::string key = option.substr(0, key_end);
        for (size_t j = 0; j < key.size(); ++j) {
          key[j] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[j])));
        }
        size_t value_begin = key_end;
        while (value_begin < option.size()
               && (std::isspace(static_cast<unsigned char>(option[value_begin]))
                   || option[value_begin] == '=')) {
          ++value_begin;
        }
        const std::string value = option.substr(value_begin);
        if ("format" == key || "type" == key) {
          std::string format = trim_sql_token(value);
          if (format.size() >= 2 && format.front() == '\'') {
            if (OB_FAIL(parse_copy_string_value(format, format))) {
            }
          }
          if (OB_SUCC(ret) && 0 != strcasecmp(format.c_str(), "csv")) {
            ret = OB_NOT_SUPPORTED;
          }
        } else if ("header" == key) {
          ret = parse_copy_bool_value(value, header);
        } else if ("delimiter" == key || "field_delimiter" == key) {
          ret = parse_copy_string_value(value, field_delimiter);
        } else if ("quote" == key || "field_enclosed_by" == key) {
          ret = parse_copy_string_value(value, field_quote);
        } else if ("escape" == key) {
          ret = parse_copy_string_value(value, escape);
        } else if ("nullstr" == key || "null_string" == key || "null" == key) {
          ret = parse_copy_string_value(value, null_string);
        } else {
          ret = OB_NOT_SUPPORTED;
        }
      }
      pos = close_pos + 1;
    }
  }
  return ret;
}

int rewrite_copy_statement(common::ObIAllocator &allocator,
                           const char *str,
                           const int64_t len,
                           const char *&rewritten,
                           int64_t &rewritten_len)
{
  int ret = OB_SUCCESS;
  rewritten = str;
  rewritten_len = len;
  int64_t pos = skip_space(str, len, 0);
  if (!match_keyword_ci(str, len, pos, "copy")) {
    // Keep ordinary statements untouched.
  } else {
    const int64_t source_begin = skip_space(str, len, pos + 4);
    const int64_t to_pos = find_query_to(str, len, source_begin);
    int64_t source_end = to_pos;
    while (source_end > source_begin
           && std::isspace(static_cast<unsigned char>(str[source_end - 1]))) {
      --source_end;
    }
    if (to_pos < 0 || source_end <= source_begin) {
      ret = OB_ERR_PARSE_SQL;
    }
    int64_t target_pos = skip_space(str, len, to_pos + 2);
    std::string target_path;
    bool quoted = false;
    bool header = false;
    std::string field_delimiter(",");
    std::string field_quote("\"");
    std::string escape("\"");
    std::string null_string;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(read_sql_string(str, len, target_pos, target_path, quoted))) {
    } else if (OB_FAIL(parse_copy_options(str, len, target_pos, header,
                                          field_delimiter, field_quote,
                                          escape, null_string))) {
    } else {
      std::string source(str + source_begin, static_cast<size_t>(source_end - source_begin));
      if (source.size() >= 2 && source.front() == '(' && source.back() == ')') {
        const int64_t close_pos =
            find_matching_paren(str, source_end, source_begin);
        if (close_pos != source_end - 1) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          source.assign(str + source_begin + 1,
                        static_cast<size_t>(source_end - source_begin - 2));
        }
      } else if (0 != source.compare(0, 6, "select")
                 && 0 != source.compare(0, 6, "SELECT")
                 && 0 != source.compare(0, 4, "with")
                 && 0 != source.compare(0, 4, "WITH")) {
        source = "SELECT * FROM " + source;
      }
      if (OB_SUCC(ret)) {
        std::string output;
        output.reserve(source.size() + target_path.size() + 256);
        output.append(source);
        output.append(" INTO OUTFILE ");
        output.append(sql_string_literal(target_path));
        output.append(" FORMAT = (TYPE = 'CSV', FIELD_DELIMITER = ");
        output.append(sql_string_literal(field_delimiter));
        output.append(", LINE_DELIMITER = '\\n', FIELD_ENCLOSED_BY = ");
        output.append(sql_string_literal(field_quote));
        output.append(", ESCAPE = ");
        output.append(sql_string_literal(escape));
        output.append(", PARSE_HEADER = ");
        output.append(header ? "TRUE" : "FALSE");
        output.append(", NULL_IF = (");
        output.append(sql_string_literal(null_string));
        output.append("))");
        char *buffer = static_cast<char *>(allocator.alloc(output.size()));
        if (OB_ISNULL(buffer)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buffer, output.data(), output.size());
          rewritten = buffer;
          rewritten_len = static_cast<int64_t>(output.size());
        }
      }
    }
  }
  return ret;
}

int rewrite_describe_file_statement(common::ObIAllocator &allocator,
                                    const char *str,
                                    const int64_t len,
                                    const char *&rewritten,
                                    int64_t &rewritten_len)
{
  int ret = OB_SUCCESS;
  rewritten = str;
  rewritten_len = len;
  int64_t pos = skip_space(str, len, 0);
  if (match_keyword_ci(str, len, pos, "describe")) {
    pos = skip_space(str, len, pos + 8);
  } else if (match_keyword_ci(str, len, pos, "desc")) {
    pos = skip_space(str, len, pos + 4);
  } else {
    return ret;
  }
  if (!match_keyword_ci(str, len, pos, "select")) {
    return ret;
  }

  const int64_t select_begin = pos;
  int64_t select_star_pos = skip_space(str, len, pos + 6);
  if (select_star_pos >= len || '*' != str[select_star_pos]) {
    select_star_pos = -1;
  }
  char quote = '\0';
  for (int64_t i = pos; OB_SUCC(ret) && i < len; ++i) {
    const char c = str[i];
    if (quote != '\0') {
      if (c == quote) {
        if (i + 1 < len && str[i + 1] == quote) {
          ++i;
        } else {
          quote = '\0';
        }
      } else if (c == '\\' && i + 1 < len) {
        ++i;
      }
      continue;
    } else if (c == '\'' || c == '"' || c == '`') {
      quote = c;
      continue;
    }

    const char *internal_name = nullptr;
    int64_t source_name_len = 0;
    if (match_function_name(str, len, i, "read_csv")) {
      internal_name = "read_csv_describe";
      source_name_len = 8;
    } else if (match_function_name(str, len, i, "read_parquet")) {
      internal_name = "read_parquet_describe";
      source_name_len = 12;
    } else if (match_function_name(str, len, i, "read_ndjson")) {
      internal_name = "read_json_describe";
      source_name_len = 11;
    } else if (match_function_name(str, len, i, "read_json")) {
      internal_name = "read_json_describe";
      source_name_len = 9;
    }

    if (nullptr != internal_name) {
      const int64_t internal_len = static_cast<int64_t>(strlen(internal_name));
      std::string output;
      output.reserve(static_cast<size_t>(len + internal_len - source_name_len + 96));
      int64_t copy_begin = select_begin;
      if (select_star_pos >= 0 && select_star_pos < i) {
        output.append(str + copy_begin,
                      static_cast<size_t>(select_star_pos - copy_begin));
        output.append("column_name, column_type, `null`, `key`, `default`, extra");
        copy_begin = select_star_pos + 1;
      }
      output.append(str + copy_begin, static_cast<size_t>(i - copy_begin));
      output.append(internal_name, static_cast<size_t>(internal_len));
      output.append(str + i + source_name_len,
                    static_cast<size_t>(len - i - source_name_len));
      char *buffer = static_cast<char *>(allocator.alloc(output.size()));
      if (OB_ISNULL(buffer)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buffer, output.data(), output.size());
        rewritten = buffer;
        rewritten_len = static_cast<int64_t>(output.size());
      }
      break;
    }
  }
  return ret;
}

} // namespace

int ObSQLParser::parse(const char * str_ptr, const int64_t str_len, ParseResult &result)
{
  int ret = OB_SUCCESS;
  const char *rewritten = str_ptr;
  int64_t rewritten_len = str_len;
  if (OB_FAIL(parse_init(&result))) {
    // do nothing
  } else if (OB_FAIL(rewrite_describe_file_statement(allocator_, str_ptr, str_len,
                                                     rewritten, rewritten_len))) {
  } else if (OB_FAIL(rewrite_copy_statement(allocator_, rewritten, rewritten_len,
                                             rewritten, rewritten_len))) {
  } else {
    if (OB_FAIL(parse_sql(&result, rewritten, static_cast<size_t>(rewritten_len)))) {
      // do nothing
    }
  }
  return ret;
}

}
}
