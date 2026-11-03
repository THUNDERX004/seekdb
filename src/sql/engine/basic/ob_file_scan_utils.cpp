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

#include "ob_file_scan_utils.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <system_error>

#include <zlib.h>

#include "lib/oblog/ob_log_module.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

namespace
{

const int64_t MAX_FILE_FIELDS = 1024;
const int64_t MAX_CSV_RECORD_SIZE = 64LL * 1024 * 1024;
const int64_t MAX_PARQUET_FOOTER_SIZE = 64LL * 1024 * 1024;
const int64_t MAX_PARQUET_PAGE_SIZE = 256LL * 1024 * 1024;
const int64_t MAX_PARQUET_CHUNK_SIZE = 512LL * 1024 * 1024;
const int64_t MAX_PARQUET_ROW_GROUP_VALUES = 100LL * 1000 * 1000;

bool is_describe_format(const FileFormat format)
{
  return FileFormat::CSV_DESCRIBE == format
      || FileFormat::JSON_DESCRIBE == format
      || FileFormat::PARQUET_DESCRIBE == format;
}

FileFormat get_describe_base_format(const FileFormat format)
{
  FileFormat base_format = FileFormat::INVALID;
  if (FileFormat::CSV_DESCRIBE == format) {
    base_format = FileFormat::CSV;
  } else if (FileFormat::JSON_DESCRIBE == format) {
    base_format = FileFormat::JSON;
  } else if (FileFormat::PARQUET_DESCRIBE == format) {
    base_format = FileFormat::PARQUET;
  }
  return base_format;
}

const char *get_column_type_name(const ColumnType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case ColumnType::BIGINT:
      name = "BIGINT";
      break;
    case ColumnType::DOUBLE:
      name = "DOUBLE";
      break;
    case ColumnType::BOOLEAN:
      name = "BOOLEAN";
      break;
    case ColumnType::DATE:
      name = "DATE";
      break;
    case ColumnType::DATETIME:
      name = "DATETIME";
      break;
    case ColumnType::VARCHAR:
    default:
      name = "VARCHAR";
      break;
  }
  return name;
}

void build_describe_schema(const TableSchema &source_schema, TableSchema &schema)
{
  schema.columns_.clear();
  const char *const names[] = {
      "column_name", "column_type", "null", "key", "default", "extra"};
  for (size_t i = 0; i < ARRAYSIZEOF(names); ++i) {
    ColumnSchema column;
    column.name_ = names[i];
    column.type_ = ColumnType::VARCHAR;
    column.nullable_ = true;
    schema.columns_.push_back(column);
  }
  schema.stat_ = source_schema.stat_;
  schema.row_count_ = static_cast<int64_t>(source_schema.columns_.size());
}

bool parse_integer(const std::string &value, int64_t &result);
bool parse_double(const std::string &value, double &result);
bool is_boolean_text(const std::string &value);
void detect_temporal(const std::string &value, bool &is_date, bool &is_datetime);

struct TypeState
{
  TypeState()
    : seen_(false),
      all_integer_(true),
      all_numeric_(true),
      all_boolean_(true),
      all_date_(true),
      all_temporal_(true)
  {}

  void add(const std::string &value)
  {
    seen_ = true;
    int64_t int_value = 0;
    double double_value = 0;
    bool is_date = false;
    bool is_datetime = false;
    const bool integer = parse_integer(value, int_value);
    const bool numeric = integer || parse_double(value, double_value);
    const bool boolean = is_boolean_text(value);
    detect_temporal(value, is_date, is_datetime);
    all_integer_ = all_integer_ && integer;
    all_numeric_ = all_numeric_ && numeric;
    all_boolean_ = all_boolean_ && boolean;
    all_date_ = all_date_ && is_date;
    all_temporal_ = all_temporal_ && (is_date || is_datetime);
  }

  ColumnType get_type() const
  {
    ColumnType type = ColumnType::VARCHAR;
    if (!seen_) {
      type = ColumnType::VARCHAR;
    } else if (all_boolean_) {
      type = ColumnType::BOOLEAN;
    } else if (all_integer_) {
      type = ColumnType::BIGINT;
    } else if (all_numeric_) {
      type = ColumnType::DOUBLE;
    } else if (all_date_) {
      type = ColumnType::DATE;
    } else if (all_temporal_) {
      type = ColumnType::DATETIME;
    }
    return type;
  }

  bool seen_;
  bool all_integer_;
  bool all_numeric_;
  bool all_boolean_;
  bool all_date_;
  bool all_temporal_;
};

struct RawField
{
  RawField() : quoted_(false) {}
  std::string value_;
  bool quoted_;
};

bool equals_ci(const std::string &left, const char *right)
{
  const size_t right_len = strlen(right);
  if (left.size() != right_len) {
    return false;
  }
  for (size_t i = 0; i < right_len; ++i) {
    const unsigned char l = static_cast<unsigned char>(left[i]);
    const unsigned char r = static_cast<unsigned char>(right[i]);
    if (std::tolower(l) != std::tolower(r)) {
      return false;
    }
  }
  return true;
}

std::string trim_copy(const std::string &value)
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

bool parse_integer(const std::string &value, int64_t &result)
{
  bool valid = false;
  if (!value.empty() && value.size() <= 20) {
    int64_t parsed = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const auto rc = std::from_chars(begin, end, parsed);
    if (rc.ec == std::errc() && rc.ptr == end) {
      result = parsed;
      valid = true;
    }
  }
  return valid;
}

bool parse_double(const std::string &value, double &result)
{
  bool valid = false;
  if (!value.empty()) {
    char *end = nullptr;
    errno = 0;
    const double parsed = strtod(value.c_str(), &end);
    if (errno == 0 && end == value.c_str() + value.size() && std::isfinite(parsed)) {
      result = parsed;
      valid = true;
    }
  }
  return valid;
}

bool is_boolean_text(const std::string &value)
{
  return equals_ci(value, "true") || equals_ci(value, "false")
      || value == "1" || value == "0";
}

bool parse_fixed_digits(const std::string &value, const size_t pos, const size_t len, int &out)
{
  bool valid = pos + len <= value.size();
  int number = 0;
  for (size_t i = 0; valid && i < len; ++i) {
    const char c = value[pos + i];
    if (c < '0' || c > '9') {
      valid = false;
    } else {
      number = number * 10 + (c - '0');
    }
  }
  if (valid) {
    out = number;
  }
  return valid;
}

bool is_leap_year(const int year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool valid_date_text(const std::string &value, const size_t pos)
{
  int year = 0;
  int month = 0;
  int day = 0;
  bool valid = pos + 10 <= value.size()
      && value[pos + 4] == '-' && value[pos + 7] == '-'
      && parse_fixed_digits(value, pos, 4, year)
      && parse_fixed_digits(value, pos + 5, 2, month)
      && parse_fixed_digits(value, pos + 8, 2, day)
      && year >= 1000 && year <= 9999 && month >= 1 && month <= 12 && day >= 1;
  static const int day_count[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (valid) {
    int max_day = day_count[month - 1] + (month == 2 && is_leap_year(year) ? 1 : 0);
    valid = day <= max_day;
  }
  return valid;
}

bool valid_datetime_text(const std::string &value)
{
  bool valid = value.size() >= 19 && valid_date_text(value, 0)
      && (value[10] == 'T' || value[10] == ' ')
      && value[13] == ':' && value[16] == ':';
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (valid) {
    valid = parse_fixed_digits(value, 11, 2, hour)
        && parse_fixed_digits(value, 14, 2, minute)
        && parse_fixed_digits(value, 17, 2, second)
        && hour <= 23 && minute <= 59 && second <= 59;
  }
  if (valid && value.size() > 19) {
    valid = value[19] == '.';
    for (size_t i = 20; valid && i < value.size(); ++i) {
      valid = value[i] >= '0' && value[i] <= '9';
    }
  }
  return valid;
}

void detect_temporal(const std::string &value, bool &is_date, bool &is_datetime)
{
  is_date = value.size() == 10 && valid_date_text(value, 0);
  is_datetime = valid_datetime_text(value);
}

int open_file(const std::string &path, FILE *&file)
{
  int ret = OB_SUCCESS;
  file = fopen(path.c_str(), "rb");
  if (NULL == file) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to open file sql input", K(ret), K(path.c_str()));
  }
  return ret;
}

void close_file(FILE *&file)
{
  if (NULL != file) {
    fclose(file);
    file = NULL;
  }
}

int64_t get_open_file_size(FILE *file)
{
  int64_t size = -1;
#ifdef _WIN32
  if (0 == _fseeki64(file, 0, SEEK_END)) {
    size = _ftelli64(file);
  }
#else
  if (0 == fseeko(file, 0, SEEK_END)) {
    size = ftello(file);
  }
#endif
  return size;
}

int seek_file(FILE *file, const int64_t offset)
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  if (0 != _fseeki64(file, offset, SEEK_SET)) {
#else
  if (0 != fseeko(file, offset, SEEK_SET)) {
#endif
    ret = OB_IO_ERROR;
  }
  return ret;
}

int read_csv_record(FILE *file,
                    const bool first_record,
                    const FileOptions &options,
                    std::vector<RawField> &fields,
                    int64_t &line_no,
                    bool &eof)
{
  int ret = OB_SUCCESS;
  fields.clear();
  eof = false;
  std::string field;
  bool in_quotes = false;
  bool after_quote = false;
  bool field_quoted = false;
  bool saw_any = false;
  int64_t record_size = 0;
  int c = 0;
  while (OB_SUCC(ret) && (c = fgetc(file)) != EOF) {
    saw_any = true;
    ++record_size;
    if (in_quotes) {
      if (c == options.quote_) {
        const int next = fgetc(file);
        if (next == options.quote_) {
          field.push_back(options.quote_);
        } else {
          in_quotes = false;
          after_quote = true;
          if (next != EOF) {
            ungetc(next, file);
          }
        }
      } else if (c == options.escape_) {
        const int next = fgetc(file);
        if (next == EOF) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("unexpected end of file after escape character", K(ret), K(line_no));
        } else if (next == 'n') {
          field.push_back('\n');
        } else if (next == 'r') {
          field.push_back('\r');
        } else if (next == 't') {
          field.push_back('\t');
        } else {
          field.push_back(static_cast<char>(next));
        }
      } else {
        if ('\n' == c) {
          ++line_no;
        }
        field.push_back(static_cast<char>(c));
      }
    } else if (after_quote) {
      if (c == options.delimiter_) {
        RawField parsed;
        parsed.value_.swap(field);
        parsed.quoted_ = field_quoted;
        fields.push_back(parsed);
        after_quote = false;
        field_quoted = false;
      } else if (c == '\n' || c == '\r') {
        if (c == '\r') {
          const int next = fgetc(file);
          if (next != '\n' && next != EOF) {
            ungetc(next, file);
          }
        }
        RawField parsed;
        parsed.value_.swap(field);
        parsed.quoted_ = field_quoted;
        fields.push_back(parsed);
        ++line_no;
        break;
      } else if (c != ' ' && c != '\t') {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("unexpected character after closing quote", K(ret), K(line_no), K(c));
      }
    } else if (c == '\n' || c == '\r') {
      if (c == '\r') {
        const int next = fgetc(file);
        if (next != '\n' && next != EOF) {
          ungetc(next, file);
        }
      }
      RawField parsed;
      parsed.value_.swap(field);
      parsed.quoted_ = field_quoted;
      fields.push_back(parsed);
      ++line_no;
      break;
    } else if (c == options.delimiter_) {
      RawField parsed;
      parsed.value_.swap(field);
      parsed.quoted_ = field_quoted;
      fields.push_back(parsed);
      field_quoted = false;
    } else if (c == options.quote_ && field.empty() && !field_quoted) {
      in_quotes = true;
      field_quoted = true;
    } else if (c == options.escape_) {
      const int next = fgetc(file);
      if (next != EOF) {
        field.push_back(static_cast<char>(next));
      }
    } else {
      field.push_back(static_cast<char>(c));
    }
    if (record_size > MAX_CSV_RECORD_SIZE
        || field.size() > MAX_CSV_RECORD_SIZE) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("csv record exceeds the configured limit", K(ret), K(line_no));
    }
  }
  if (OB_SUCC(ret) && !saw_any) {
    eof = true;
  } else if (OB_SUCC(ret) && in_quotes) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unterminated csv quoted field", K(ret), K(line_no));
  } else if (OB_SUCC(ret) && !fields.empty() && !(fields.size() == 1 && fields[0].value_.empty()
      && !fields[0].quoted_ && line_no == 0 && first_record)) {
    // The completion path above already appended the final field.
  } else if (OB_SUCC(ret) && saw_any && fields.empty()) {
    RawField parsed;
    parsed.value_.swap(field);
    parsed.quoted_ = field_quoted;
    fields.push_back(parsed);
    ++line_no;
  }
  if (OB_SUCC(ret) && fields.size() > MAX_FILE_FIELDS) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("csv contains too many columns", K(ret), K(fields.size()));
  }
  return ret;
}

std::vector<std::string> make_column_names(const std::vector<RawField> &header)
{
  std::vector<std::string> names;
  names.reserve(header.size());
  for (size_t i = 0; i < header.size(); ++i) {
    std::string name = header[i].value_;
    if (0 == i && name.size() >= 3
        && static_cast<unsigned char>(name[0]) == 0xef
        && static_cast<unsigned char>(name[1]) == 0xbb
        && static_cast<unsigned char>(name[2]) == 0xbf) {
      name.erase(0, 3);
    }
    if (name.empty()) {
      name = "column" + std::to_string(i + 1);
    }
    int suffix = 2;
    std::string candidate = name;
    bool duplicate = true;
    while (duplicate) {
      duplicate = false;
      for (size_t j = 0; j < names.size(); ++j) {
        if (equals_ci(names[j], candidate.c_str())) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        candidate = name + "__" + std::to_string(suffix++);
      }
    }
    names.push_back(candidate);
  }
  return names;
}

int infer_csv_schema(const std::string &path,
                     const FileOptions &options,
                     TableSchema &schema)
{
  int ret = OB_SUCCESS;
  FILE *file = NULL;
  std::vector<RawField> fields;
  std::vector<std::string> names;
  std::vector<TypeState> states;
  std::vector<bool> nullable;
  int64_t line_no = 0;
  bool eof = false;
  bool first_data_record = true;
  if (OB_FAIL(open_file(path, file))) {
  } else if (OB_FAIL(read_csv_record(file, true, options, fields, line_no, eof))) {
  } else if (eof || fields.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("csv file has no header/data", K(ret), K(path.c_str()));
  } else {
    if (options.header_) {
      names = make_column_names(fields);
    } else {
      for (size_t i = 0; i < fields.size(); ++i) {
        names.push_back("column" + std::to_string(i + 1));
      }
      first_data_record = false;
    }
    states.resize(names.size());
    nullable.assign(names.size(), false);
    if (!options.header_) {
      for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].quoted_
            || (!fields[i].value_.empty()
                && (options.null_string_.empty()
                    || fields[i].value_ != options.null_string_))) {
          states[i].add(fields[i].value_);
        } else {
          nullable[i] = true;
        }
      }
    }
    while (OB_SUCC(ret) && !eof) {
      if (OB_FAIL(read_csv_record(file, false, options, fields, line_no, eof))) {
      } else if (eof) {
        break;
      } else if (fields.size() != names.size()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("csv row has a different column count", K(ret), K(line_no),
                 K(fields.size()), K(names.size()));
      } else {
        for (size_t i = 0; OB_SUCC(ret) && i < fields.size(); ++i) {
          if (!fields[i].quoted_
              && (fields[i].value_.empty()
                  || (!options.null_string_.empty()
                      && fields[i].value_ == options.null_string_))) {
            nullable[i] = true;
          } else {
            states[i].add(fields[i].value_);
          }
        }
        ++schema.row_count_;
      }
    }
  }
  close_file(file);
  if (OB_SUCC(ret)) {
    schema.columns_.clear();
    for (size_t i = 0; i < names.size(); ++i) {
      ColumnSchema column;
      column.name_ = names[i];
      column.type_ = states[i].get_type();
      column.nullable_ = nullable[i] || !states[i].seen_;
      schema.columns_.push_back(column);
    }
  }
  return ret;
}

struct JsonValue
{
  enum Type
  {
    INVALID,
    NUL,
    BOOLEAN,
    NUMBER,
    STRING,
    ARRAY,
    OBJECT,
  };

  JsonValue() : type_(INVALID), bool_value_(false) {}

  Type type_;
  bool bool_value_;
  std::string scalar_;
  std::vector<JsonValue> array_;
  std::vector<std::pair<std::string, JsonValue>> object_;
};

class JsonParser
{
public:
  explicit JsonParser(const std::string &text) : text_(text), pos_(0), depth_(0) {}

  int parse(JsonValue &value)
  {
    int ret = OB_SUCCESS;
    skip_space();
    if (OB_FAIL(parse_value(value))) {
    } else {
      skip_space();
      if (pos_ != text_.size()) {
        ret = OB_ERR_JSON_SYNTAX_ERROR;
      }
    }
    return ret;
  }

private:
  void skip_space()
  {
    while (pos_ < text_.size()
           && (text_[pos_] == ' ' || text_[pos_] == '\t'
               || text_[pos_] == '\r' || text_[pos_] == '\n')) {
      ++pos_;
    }
  }

  int parse_value(JsonValue &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= text_.size()) {
      ret = OB_ERR_JSON_SYNTAX_ERROR;
    } else if (text_[pos_] == '{') {
      ret = parse_object(value);
    } else if (text_[pos_] == '[') {
      ret = parse_array(value);
    } else if (text_[pos_] == '"') {
      value.type_ = JsonValue::STRING;
      ret = parse_string(value.scalar_);
    } else if (text_.compare(pos_, 4, "true") == 0) {
      value.type_ = JsonValue::BOOLEAN;
      value.bool_value_ = true;
      pos_ += 4;
    } else if (text_.compare(pos_, 5, "false") == 0) {
      value.type_ = JsonValue::BOOLEAN;
      value.bool_value_ = false;
      pos_ += 5;
    } else if (text_.compare(pos_, 4, "null") == 0) {
      value.type_ = JsonValue::NUL;
      pos_ += 4;
    } else {
      ret = parse_number(value.scalar_);
      if (OB_SUCC(ret)) {
        value.type_ = JsonValue::NUMBER;
      }
    }
    return ret;
  }

  int parse_object(JsonValue &value)
  {
    int ret = OB_SUCCESS;
    if (depth_ >= 1) {
      ret = OB_NOT_SUPPORTED;
    }
    ++depth_;
    value.type_ = JsonValue::OBJECT;
    ++pos_;
    skip_space();
    if (OB_FAIL(ret)) {
    } else if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
    } else {
      while (OB_SUCC(ret)) {
        std::string key;
        JsonValue child;
        skip_space();
        if (pos_ >= text_.size() || text_[pos_] != '"') {
          ret = OB_ERR_JSON_SYNTAX_ERROR;
        } else if (OB_FAIL(parse_string(key))) {
        } else {
          skip_space();
          if (pos_ >= text_.size() || text_[pos_] != ':') {
            ret = OB_ERR_JSON_SYNTAX_ERROR;
          } else {
            ++pos_;
            skip_space();
            if (OB_FAIL(parse_value(child))) {
            } else {
              value.object_.push_back(std::make_pair(key, child));
              skip_space();
              if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
              } else if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                break;
              } else {
                ret = OB_ERR_JSON_SYNTAX_ERROR;
              }
            }
          }
        }
      }
    }
    --depth_;
    return ret;
  }

  int parse_array(JsonValue &value)
  {
    int ret = OB_SUCCESS;
    if (depth_ >= 1) {
      ret = OB_NOT_SUPPORTED;
    }
    ++depth_;
    value.type_ = JsonValue::ARRAY;
    ++pos_;
    skip_space();
    if (OB_FAIL(ret)) {
    } else if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
    } else {
      while (OB_SUCC(ret)) {
        JsonValue child;
        if (OB_FAIL(parse_value(child))) {
        } else {
          value.array_.push_back(child);
          skip_space();
          if (pos_ < text_.size() && text_[pos_] == ',') {
            ++pos_;
            skip_space();
          } else if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            break;
          } else {
            ret = OB_ERR_JSON_SYNTAX_ERROR;
          }
        }
      }
    }
    --depth_;
    return ret;
  }

  int parse_string(std::string &value)
  {
    int ret = OB_SUCCESS;
    value.clear();
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      ret = OB_ERR_JSON_SYNTAX_ERROR;
    } else {
      ++pos_;
      bool done = false;
      while (OB_SUCC(ret) && !done && pos_ < text_.size()) {
        const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
        if (c == '"') {
          done = true;
        } else if (c == '\\') {
          if (pos_ >= text_.size()) {
            ret = OB_ERR_JSON_SYNTAX_ERROR;
          } else {
            const char escaped = text_[pos_++];
            if (escaped == '"' || escaped == '\\' || escaped == '/') {
              value.push_back(escaped);
            } else if (escaped == 'b') {
              value.push_back('\b');
            } else if (escaped == 'f') {
              value.push_back('\f');
            } else if (escaped == 'n') {
              value.push_back('\n');
            } else if (escaped == 'r') {
              value.push_back('\r');
            } else if (escaped == 't') {
              value.push_back('\t');
            } else if (escaped == 'u') {
              int codepoint = 0;
              bool valid = pos_ + 4 <= text_.size();
              for (size_t i = 0; valid && i < 4; ++i) {
                const char h = text_[pos_ + i];
                int digit = -1;
                if (h >= '0' && h <= '9') {
                  digit = h - '0';
                } else if (h >= 'a' && h <= 'f') {
                  digit = h - 'a' + 10;
                } else if (h >= 'A' && h <= 'F') {
                  digit = h - 'A' + 10;
                }
                valid = digit >= 0;
                if (valid) {
                  codepoint = codepoint * 16 + digit;
                }
              }
              if (!valid) {
                ret = OB_ERR_JSON_SYNTAX_ERROR;
              } else {
                pos_ += 4;
                if (codepoint <= 0x7f) {
                  value.push_back(static_cast<char>(codepoint));
                } else if (codepoint <= 0x7ff) {
                  value.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
                  value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
                } else {
                  value.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
                  value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
                  value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
                }
              }
            } else {
              ret = OB_ERR_JSON_SYNTAX_ERROR;
            }
          }
        } else if (c < 0x20) {
          ret = OB_ERR_JSON_SYNTAX_ERROR;
        } else {
          value.push_back(static_cast<char>(c));
        }
      }
      if (OB_SUCC(ret) && !done) {
        ret = OB_ERR_JSON_SYNTAX_ERROR;
      }
    }
    return ret;
  }

  int parse_number(std::string &value)
  {
    int ret = OB_SUCCESS;
    const size_t begin = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') {
      ++pos_;
    }
    if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
      ret = OB_ERR_JSON_SYNTAX_ERROR;
    } else if (text_[pos_] == '0') {
      ++pos_;
    } else {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (OB_SUCC(ret) && pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        ret = OB_ERR_JSON_SYNTAX_ERROR;
      } else {
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
          ++pos_;
        }
      }
    }
    if (OB_SUCC(ret) && pos_ < text_.size()
        && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        ret = OB_ERR_JSON_SYNTAX_ERROR;
      } else {
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
          ++pos_;
        }
      }
    }
    if (OB_SUCC(ret)) {
      value.assign(text_, begin, pos_ - begin);
    }
    return ret;
  }

  const std::string &text_;
  size_t pos_;
  int64_t depth_;
};

std::string json_scalar_text(const JsonValue &value)
{
  std::string text;
  if (JsonValue::BOOLEAN == value.type_) {
    text = value.bool_value_ ? "1" : "0";
  } else {
    text = value.scalar_;
  }
  return text;
}

const JsonValue *find_json_field(const JsonValue &object, const std::string &name)
{
  const JsonValue *result = nullptr;
  for (auto it = object.object_.rbegin(); it != object.object_.rend(); ++it) {
    if (it->first == name) {
      result = &it->second;
      break;
    }
  }
  return result;
}

int read_json_line(FILE *file, std::string &line, int64_t &line_no, bool &eof)
{
  int ret = OB_SUCCESS;
  line.clear();
  eof = false;
  int c = 0;
  bool saw_any = false;
  while ((c = fgetc(file)) != EOF) {
    saw_any = true;
    if (c == '\n') {
      break;
    } else if (c == '\r') {
      const int next = fgetc(file);
      if (next != '\n' && next != EOF) {
        ungetc(next, file);
      }
      break;
    } else {
      line.push_back(static_cast<char>(c));
    }
    if (line.size() > MAX_CSV_RECORD_SIZE) {
      ret = OB_SIZE_OVERFLOW;
      break;
    }
  }
  if (!saw_any) {
    eof = true;
  } else {
    ++line_no;
  }
  return ret;
}

int flatten_json_object(const JsonValue &object,
                        std::vector<std::pair<std::string, std::string>> &fields,
                        const int64_t line_no)
{
  int ret = OB_SUCCESS;
  if (JsonValue::OBJECT != object.type_) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("jsonl record must be an object", K(ret), K(line_no));
  } else if (object.object_.size() > MAX_FILE_FIELDS) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("jsonl record contains too many fields", K(ret), K(line_no),
             K(object.object_.size()));
  } else {
    for (size_t i = 0; OB_SUCC(ret) && i < object.object_.size(); ++i) {
      const JsonValue &value = object.object_[i].second;
      if (JsonValue::OBJECT == value.type_ || JsonValue::ARRAY == value.type_) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("nested json values are not supported", K(ret), K(line_no),
                 K(object.object_[i].first.c_str()));
      } else if (JsonValue::NUL == value.type_) {
        fields.push_back(std::make_pair(object.object_[i].first, std::string()));
      } else {
        fields.push_back(std::make_pair(object.object_[i].first, json_scalar_text(value)));
      }
    }
  }
  return ret;
}

int infer_json_schema(const std::string &path,
                      TableSchema &schema)
{
  int ret = OB_SUCCESS;
  FILE *file = NULL;
  std::string line;
  int64_t line_no = 0;
  bool eof = false;
  std::vector<std::string> names;
  std::vector<TypeState> states;
  std::vector<bool> nullable;
  if (OB_FAIL(open_file(path, file))) {
  }
  while (OB_SUCC(ret) && !eof) {
    if (OB_FAIL(read_json_line(file, line, line_no, eof))) {
    } else if (eof) {
      break;
    } else if (trim_copy(line).empty()) {
      continue;
    } else {
      JsonValue object;
      std::vector<std::pair<std::string, std::string>> fields;
      JsonParser parser(line);
      if (OB_FAIL(parser.parse(object))) {
        LOG_WARN("failed to parse jsonl record", K(ret), K(line_no));
      } else if (OB_FAIL(flatten_json_object(object, fields, line_no))) {
      } else {
        for (size_t i = 0; i < fields.size(); ++i) {
          int64_t column_idx = -1;
          for (size_t j = 0; j < names.size(); ++j) {
            if (names[j] == fields[i].first) {
              column_idx = static_cast<int64_t>(j);
              break;
            }
          }
          if (column_idx < 0) {
            column_idx = static_cast<int64_t>(names.size());
            names.push_back(fields[i].first);
            states.push_back(TypeState());
            nullable.push_back(false);
          }
          if (fields[i].second.empty()
              && find_json_field(object, fields[i].first)->type_ == JsonValue::NUL) {
            nullable[column_idx] = true;
          } else {
            states[column_idx].add(fields[i].second);
          }
        }
        for (size_t i = 0; i < names.size(); ++i) {
          const JsonValue *field = find_json_field(object, names[i]);
          if (nullptr == field || JsonValue::NUL == field->type_) {
            nullable[i] = true;
          }
        }
        ++schema.row_count_;
      }
    }
  }
  close_file(file);
  if (OB_SUCC(ret) && names.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("jsonl file has no columns", K(ret), K(path.c_str()));
  }
  if (OB_SUCC(ret)) {
    schema.columns_.clear();
    for (size_t i = 0; i < names.size(); ++i) {
      ColumnSchema column;
      column.name_ = names[i];
      column.type_ = states[i].get_type();
      column.nullable_ = nullable[i] || !states[i].seen_;
      schema.columns_.push_back(column);
    }
  }
  return ret;
}

class CsvReader final : public IFileReader
{
public:
  CsvReader(const std::string &path,
            const FileOptions &options,
            const TableSchema &schema)
    : path_(path), options_(options), schema_(schema), file_(NULL),
      line_no_(0), skipped_header_(false)
  {}
  virtual ~CsvReader() { close_file(file_); }
  virtual int rescan() override
  {
    int ret = OB_SUCCESS;
    close_file(file_);
    line_no_ = 0;
    skipped_header_ = false;
    if (OB_FAIL(open_file(path_, file_))) {
    }
    return ret;
  }
  virtual int get_next_row(std::vector<ScalarValue> &row) override
  {
    int ret = OB_SUCCESS;
    std::vector<RawField> fields;
    bool eof = false;
    if (NULL == file_) {
      ret = OB_NOT_INIT;
    } else if (!options_.header_ || skipped_header_) {
      if (OB_FAIL(read_csv_record(file_, false, options_, fields, line_no_, eof))) {
      }
    } else {
      int64_t header_line = 0;
      if (OB_FAIL(read_csv_record(file_, true, options_, fields, header_line, eof))) {
      } else if (eof) {
        ret = OB_ITER_END;
      } else {
        line_no_ = header_line;
        skipped_header_ = true;
        fields.clear();
        if (OB_FAIL(read_csv_record(file_, false, options_, fields, line_no_, eof))) {
        }
      }
    }
    if (OB_SUCC(ret) && eof) {
      ret = OB_ITER_END;
    } else if (OB_SUCC(ret) && fields.size() != schema_.columns_.size()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("csv row column count mismatch", K(ret), K(line_no_),
               K(fields.size()), K(schema_.columns_.size()));
    } else if (OB_SUCC(ret)) {
      row.clear();
      row.resize(fields.size());
      for (size_t i = 0; i < fields.size(); ++i) {
        if (!fields[i].quoted_
            && (fields[i].value_.empty()
                || (!options_.null_string_.empty()
                    && fields[i].value_ == options_.null_string_))) {
          row[i].is_null_ = true;
        } else {
          row[i].is_null_ = false;
          row[i].text_ = std::move(fields[i].value_);
        }
      }
    }
    return ret;
  }

private:
  std::string path_;
  FileOptions options_;
  TableSchema schema_;
  FILE *file_;
  int64_t line_no_;
  bool skipped_header_;
};

class JsonReader final : public IFileReader
{
public:
  JsonReader(const std::string &path, const TableSchema &schema)
    : path_(path), schema_(schema), file_(NULL), line_no_(0)
  {}
  virtual ~JsonReader() { close_file(file_); }
  virtual int rescan() override
  {
    int ret = OB_SUCCESS;
    close_file(file_);
    line_no_ = 0;
    if (OB_FAIL(open_file(path_, file_))) {
    }
    return ret;
  }
  virtual int get_next_row(std::vector<ScalarValue> &row) override
  {
    int ret = OB_SUCCESS;
    std::string line;
    bool eof = false;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(read_json_line(file_, line, line_no_, eof))) {
      } else if (eof) {
        ret = OB_ITER_END;
      } else if (trim_copy(line).empty()) {
        continue;
      } else {
        JsonValue object;
        JsonParser parser(line);
        if (OB_FAIL(parser.parse(object))) {
          LOG_WARN("failed to parse jsonl record", K(ret), K(line_no_));
        } else if (JsonValue::OBJECT != object.type_) {
          ret = OB_NOT_SUPPORTED;
        } else {
          row.clear();
          row.resize(schema_.columns_.size());
          for (size_t i = 0; i < schema_.columns_.size(); ++i) {
            const JsonValue *value = find_json_field(object, schema_.columns_[i].name_);
            if (nullptr == value || JsonValue::NUL == value->type_) {
              row[i].is_null_ = true;
            } else if (JsonValue::OBJECT == value->type_ || JsonValue::ARRAY == value->type_) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("nested json values are not supported", K(ret), K(line_no_));
            } else {
              row[i].is_null_ = false;
              row[i].text_ = json_scalar_text(*value);
            }
          }
          break;
        }
      }
    }
    return ret;
  }

private:
  std::string path_;
  TableSchema schema_;
  FILE *file_;
  int64_t line_no_;
};

class GlobReader final : public IFileReader
{
public:
  explicit GlobReader(const std::string &pattern)
    : pattern_(pattern), current_(0)
  {}
  virtual int rescan() override
  {
    int ret = OB_SUCCESS;
    files_.clear();
    current_ = 0;
    ret = list_glob_files(pattern_, files_);
    return ret;
  }
  virtual int get_next_row(std::vector<ScalarValue> &row) override
  {
    int ret = OB_SUCCESS;
    if (current_ >= files_.size()) {
      ret = OB_ITER_END;
    } else {
      row.clear();
      row.resize(1);
      row[0].is_null_ = false;
      row[0].text_ = files_[current_++];
    }
    return ret;
  }

private:
  std::string pattern_;
  std::vector<std::string> files_;
  size_t current_;
};

class DescribeReader final : public IFileReader
{
public:
  explicit DescribeReader(const TableSchema &schema)
    : schema_(schema), current_(0)
  {}
  virtual int rescan() override
  {
    current_ = 0;
    return OB_SUCCESS;
  }
  virtual int get_next_row(std::vector<ScalarValue> &row) override
  {
    int ret = OB_SUCCESS;
    if (current_ >= schema_.columns_.size()) {
      ret = OB_ITER_END;
    } else {
      const ColumnSchema &column = schema_.columns_[current_++];
      row.clear();
      row.resize(6);
      row[0].is_null_ = false;
      row[0].text_ = column.name_;
      row[1].is_null_ = false;
      row[1].text_ = get_column_type_name(column.type_);
      row[2].is_null_ = false;
      row[2].text_ = column.nullable_ ? "YES" : "NO";
      row[3].is_null_ = true;
      row[4].is_null_ = true;
      row[5].is_null_ = true;
    }
    return ret;
  }

private:
  TableSchema schema_;
  size_t current_;
};

struct ThriftValue
{
  enum Kind
  {
    INVALID,
    BOOL,
    INTEGER,
    DOUBLE,
    BINARY,
    LIST,
    STRUCT,
  };

  ThriftValue() : kind_(INVALID), bool_(false), integer_(0), double_(0) {}

  const ThriftValue *field(const int16_t id) const
  {
    for (size_t i = 0; i < fields_.size(); ++i) {
      if (fields_[i].first == id) {
        return &fields_[i].second;
      }
    }
    return nullptr;
  }

  int64_t integer_or(const int16_t id, const int64_t default_value) const
  {
    const ThriftValue *value = field(id);
    return nullptr == value ? default_value : value->integer_;
  }

  Kind kind_;
  bool bool_;
  int64_t integer_;
  double double_;
  std::string binary_;
  std::vector<ThriftValue> list_;
  std::vector<std::pair<int16_t, ThriftValue>> fields_;
};

class ThriftCompactReader
{
public:
  ThriftCompactReader(const uint8_t *data, const int64_t size)
    : data_(data), size_(size), pos_(0)
  {}

  int64_t position() const { return pos_; }

  int read_struct(ThriftValue &value)
  {
    int ret = OB_SUCCESS;
    value.kind_ = ThriftValue::STRUCT;
    int16_t last_field_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t header = 0;
      if (OB_FAIL(read_byte(header))) {
      } else if (0 == header) {
        break;
      } else {
        int16_t field_id = static_cast<int16_t>(header >> 4);
        const uint8_t type = header & 0x0f;
        if (0 == field_id) {
          int64_t delta = 0;
          if (OB_FAIL(read_zigzag(delta))) {
          } else {
            field_id = static_cast<int16_t>(delta);
          }
        } else {
          field_id = static_cast<int16_t>(last_field_id + field_id);
        }
        if (OB_SUCC(ret)) {
          ThriftValue field_value;
          if (OB_FAIL(read_value(type, field_value))) {
          } else {
            value.fields_.push_back(std::make_pair(field_id, field_value));
            last_field_id = field_id;
          }
        }
      }
    }
    return ret;
  }

private:
  int read_byte(uint8_t &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= size_) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      value = data_[pos_++];
    }
    return ret;
  }

  int read_bytes(void *dst, const int64_t len)
  {
    int ret = OB_SUCCESS;
    if (len < 0 || pos_ + len > size_) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      memcpy(dst, data_ + pos_, static_cast<size_t>(len));
      pos_ += len;
    }
    return ret;
  }

  int read_varint(uint64_t &value)
  {
    int ret = OB_SUCCESS;
    value = 0;
    int shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      uint8_t byte = 0;
      if (shift >= 64 || OB_FAIL(read_byte(byte))) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        done = 0 == (byte & 0x80);
        shift += 7;
      }
    }
    return ret;
  }

  int read_zigzag(int64_t &value)
  {
    uint64_t encoded = 0;
    int ret = read_varint(encoded);
    if (OB_SUCC(ret)) {
      value = static_cast<int64_t>((encoded >> 1) ^ (~(encoded & 1) + 1));
    }
    return ret;
  }

  int read_value(const uint8_t type, ThriftValue &value)
  {
    int ret = OB_SUCCESS;
    switch (type) {
      case 1:
        value.kind_ = ThriftValue::BOOL;
        value.bool_ = true;
        break;
      case 2:
        value.kind_ = ThriftValue::BOOL;
        value.bool_ = false;
        break;
      case 3: {
        uint8_t byte = 0;
        if (OB_FAIL(read_byte(byte))) {
        } else {
          value.kind_ = ThriftValue::INTEGER;
          value.integer_ = static_cast<int8_t>(byte);
        }
        break;
      }
      case 4:
      case 5:
      case 6:
        value.kind_ = ThriftValue::INTEGER;
        ret = read_zigzag(value.integer_);
        break;
      case 7: {
        uint64_t bits = 0;
        value.kind_ = ThriftValue::DOUBLE;
        if (OB_FAIL(read_bytes(&bits, sizeof(bits)))) {
        } else {
          memcpy(&value.double_, &bits, sizeof(value.double_));
        }
        break;
      }
      case 8: {
        uint64_t len = 0;
        value.kind_ = ThriftValue::BINARY;
        if (OB_FAIL(read_varint(len)) || len > static_cast<uint64_t>(size_ - pos_)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          value.binary_.assign(reinterpret_cast<const char *>(data_ + pos_),
                               static_cast<size_t>(len));
          pos_ += static_cast<int64_t>(len);
        }
        break;
      }
      case 9:
      case 10:
        ret = read_list(value);
        break;
      case 11:
        ret = read_map(value);
        break;
      case 12:
        ret = read_struct(value);
        break;
      default:
        ret = OB_INVALID_ARGUMENT;
        break;
    }
    return ret;
  }

  int read_list(ThriftValue &value)
  {
    int ret = OB_SUCCESS;
    uint8_t header = 0;
    value.kind_ = ThriftValue::LIST;
    if (OB_FAIL(read_byte(header))) {
    } else {
      uint64_t count = header >> 4;
      const uint8_t element_type = header & 0x0f;
      if (15 == count && OB_FAIL(read_varint(count))) {
      } else if (count > static_cast<uint64_t>(size_)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        value.list_.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ThriftValue element;
          if (1 == element_type || 2 == element_type) {
            uint8_t bool_byte = 0;
            if (OB_FAIL(read_byte(bool_byte))) {
            } else {
              element.kind_ = ThriftValue::BOOL;
              element.bool_ = 1 == bool_byte;
            }
          } else if (OB_FAIL(read_value(element_type, element))) {
          }
          if (OB_SUCC(ret)) {
            value.list_.push_back(std::move(element));
          }
        }
      }
    }
    return ret;
  }

  int read_map(ThriftValue &value)
  {
    int ret = OB_SUCCESS;
    uint64_t count = 0;
    value.kind_ = ThriftValue::STRUCT;
    if (OB_FAIL(read_varint(count))) {
    } else if (count > 0) {
      uint8_t types = 0;
      if (OB_FAIL(read_byte(types))) {
      } else {
        const uint8_t key_type = types >> 4;
        const uint8_t value_type = types & 0x0f;
        for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ThriftValue key;
          ThriftValue map_value;
          if (OB_FAIL(read_value(key_type, key)) || OB_FAIL(read_value(value_type, map_value))) {
          } else {
            value.fields_.push_back(std::make_pair(
                static_cast<int16_t>(key.integer_), std::move(map_value)));
          }
        }
      }
    }
    return ret;
  }

  const uint8_t *data_;
  int64_t size_;
  int64_t pos_;
};

uint32_t read_u32_le(const uint8_t *data)
{
  return static_cast<uint32_t>(data[0])
      | (static_cast<uint32_t>(data[1]) << 8)
      | (static_cast<uint32_t>(data[2]) << 16)
      | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t read_u64_le(const uint8_t *data)
{
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | data[i];
  }
  return value;
}

struct ParquetSchemaElement
{
  ParquetSchemaElement()
    : physical_type_(-1), repetition_type_(0), converted_type_(-1), num_children_(0)
  {}
  int32_t physical_type_;
  int32_t repetition_type_;
  int32_t converted_type_;
  int32_t num_children_;
  std::string name_;
};

struct ParquetColumnMeta
{
  ParquetColumnMeta()
    : physical_type_(-1), codec_(0), num_values_(0), total_compressed_size_(0),
      data_page_offset_(0), dictionary_page_offset_(0)
  {}
  int32_t physical_type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t total_compressed_size_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  std::vector<std::string> path_;
};

struct ParquetRowGroup
{
  ParquetRowGroup() : num_rows_(0), total_byte_size_(0) {}
  int64_t num_rows_;
  int64_t total_byte_size_;
  std::vector<ParquetColumnMeta> columns_;
};

struct ParquetFileMeta
{
  ParquetFileMeta() : num_rows_(0) {}
  int64_t num_rows_;
  std::vector<ParquetSchemaElement> schema_;
  std::vector<ParquetRowGroup> row_groups_;
};

int parse_parquet_meta(const std::vector<uint8_t> &footer, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  const char *stage = "footer";
  ThriftValue root;
  ThriftCompactReader reader(footer.data(), static_cast<int64_t>(footer.size()));
  if (footer.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(reader.read_struct(root))) {
    stage = "thrift_struct";
  } else {
    stage = "file_meta_fields";
    const ThriftValue *num_rows = root.field(3);
    const ThriftValue *schema = root.field(2);
    const ThriftValue *row_groups = root.field(4);
    if (nullptr == num_rows || nullptr == schema || nullptr == row_groups) {
      ret = OB_INVALID_ARGUMENT;
    } else if (num_rows->integer_ < 0
               || schema->list_.size() > static_cast<size_t>(MAX_FILE_FIELDS + 1)
               || row_groups->list_.size() > 1000000) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      stage = "schema";
      meta.num_rows_ = num_rows->integer_;
      for (size_t i = 0; OB_SUCC(ret) && i < schema->list_.size(); ++i) {
        const ThriftValue &schema_value = schema->list_[i];
        ParquetSchemaElement element;
        const ThriftValue *type = schema_value.field(1);
        const ThriftValue *repetition = schema_value.field(3);
        const ThriftValue *name = schema_value.field(4);
        const ThriftValue *children = schema_value.field(5);
        const ThriftValue *converted = schema_value.field(6);
        if (nullptr == name) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          element.physical_type_ = nullptr == type ? -1 : static_cast<int32_t>(type->integer_);
          element.repetition_type_ =
              nullptr == repetition ? 0 : static_cast<int32_t>(repetition->integer_);
          element.num_children_ =
              nullptr == children ? 0 : static_cast<int32_t>(children->integer_);
          element.converted_type_ =
              nullptr == converted ? -1 : static_cast<int32_t>(converted->integer_);
          element.name_ = name->binary_;
          meta.schema_.push_back(element);
        }
      }
      stage = "row_groups";
      for (size_t i = 0; OB_SUCC(ret) && i < row_groups->list_.size(); ++i) {
        const ThriftValue &row_group_value = row_groups->list_[i];
        ParquetRowGroup row_group;
        const ThriftValue *rows = row_group_value.field(3);
        const ThriftValue *byte_size = row_group_value.field(2);
        const ThriftValue *columns = row_group_value.field(1);
        if (nullptr == rows || nullptr == columns) {
          ret = OB_INVALID_ARGUMENT;
        } else if (rows->integer_ < 0
                   || rows->integer_ > MAX_PARQUET_ROW_GROUP_VALUES
                   || columns->list_.size() > static_cast<size_t>(MAX_FILE_FIELDS)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          row_group.num_rows_ = rows->integer_;
          row_group.total_byte_size_ =
              nullptr == byte_size ? 0 : byte_size->integer_;
          for (size_t j = 0; OB_SUCC(ret) && j < columns->list_.size(); ++j) {
            stage = "column_meta";
            const ThriftValue &column_value = columns->list_[j];
            const ThriftValue *column_meta_value = column_value.field(3);
            if (nullptr == column_meta_value) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              ParquetColumnMeta column;
              const ThriftValue *type = column_meta_value->field(1);
              const ThriftValue *path = column_meta_value->field(3);
              const ThriftValue *codec = column_meta_value->field(4);
              const ThriftValue *num_values = column_meta_value->field(5);
              const ThriftValue *compressed_size = column_meta_value->field(7);
              const ThriftValue *data_offset = column_meta_value->field(9);
              const ThriftValue *dictionary_offset = column_meta_value->field(11);
              if (nullptr == type || nullptr == path || nullptr == num_values
                  || nullptr == compressed_size || nullptr == data_offset) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                column.physical_type_ = static_cast<int32_t>(type->integer_);
                column.codec_ = nullptr == codec ? 0 : static_cast<int32_t>(codec->integer_);
                column.num_values_ = num_values->integer_;
                column.total_compressed_size_ = compressed_size->integer_;
                column.data_page_offset_ = data_offset->integer_;
                column.dictionary_page_offset_ =
                    nullptr == dictionary_offset ? 0 : dictionary_offset->integer_;
                for (size_t k = 0; k < path->list_.size(); ++k) {
                  column.path_.push_back(path->list_[k].binary_);
                }
                row_group.columns_.push_back(column);
              }
            }
          }
          meta.row_groups_.push_back(row_group);
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("failed to parse parquet metadata", K(ret), K(stage),
             K(meta.num_rows_), K(meta.schema_.size()), K(meta.row_groups_.size()));
  }
  return ret;
}

int read_parquet_footer(FILE *file, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  const int64_t file_size = get_open_file_size(file);
  if (file_size < 0) {
    ret = OB_IO_ERROR;
  } else {
    if (file_size < 12) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      uint8_t trailer[8] = {0};
      if (OB_FAIL(seek_file(file, file_size - 8))
          || 8 != fread(trailer, 1, 8, file)
          || memcmp(trailer + 4, "PAR1", 4) != 0) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const uint32_t footer_size = read_u32_le(trailer);
        if (footer_size > static_cast<uint32_t>(file_size - 8)
            || footer_size > static_cast<uint32_t>(MAX_PARQUET_FOOTER_SIZE)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          std::vector<uint8_t> footer(footer_size);
          if (OB_FAIL(seek_file(file, file_size - 8 - footer_size))
              || footer_size != fread(footer.data(), 1, footer_size, file)) {
            ret = OB_IO_ERROR;
          } else {
            ret = parse_parquet_meta(footer, meta);
          }
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("failed to read parquet footer", K(ret));
  }
  return ret;
}

ColumnType parquet_column_type(const ParquetSchemaElement &element)
{
  ColumnType type = ColumnType::VARCHAR;
  switch (element.physical_type_) {
    case 0:
      type = ColumnType::BOOLEAN;
      break;
    case 1:
      type = 6 == element.converted_type_ ? ColumnType::DATE : ColumnType::BIGINT;
      break;
    case 2:
    case 3:
      type = (9 == element.converted_type_ || 10 == element.converted_type_)
          ? ColumnType::DATETIME : ColumnType::BIGINT;
      break;
    case 4:
    case 5:
      type = ColumnType::DOUBLE;
      break;
    default:
      type = ColumnType::VARCHAR;
      break;
  }
  return type;
}

int infer_parquet_schema(FILE *file, TableSchema &schema)
{
  int ret = OB_SUCCESS;
  ParquetFileMeta meta;
  if (OB_FAIL(read_parquet_footer(file, meta))) {
  } else {
    for (size_t i = 1; OB_SUCC(ret) && i < meta.schema_.size(); ++i) {
      const ParquetSchemaElement &element = meta.schema_[i];
      if (element.physical_type_ >= 0 && element.num_children_ == 0) {
        ColumnSchema column;
        column.name_ = element.name_;
        column.type_ = parquet_column_type(element);
        column.nullable_ = 1 == element.repetition_type_;
        schema.columns_.push_back(column);
      }
    }
    schema.row_count_ = meta.num_rows_;
    if (schema.columns_.empty()) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("failed to infer parquet schema", K(ret),
             K(schema.columns_.size()), K(schema.row_count_));
  }
  return ret;
}

int snappy_decompress(const uint8_t *input,
                      const int64_t input_size,
                      std::vector<uint8_t> &output)
{
  int ret = OB_SUCCESS;
  const char *stage = "size";
  int64_t pos = 0;
  uint64_t expected_size = 0;
  int shift = 0;
  bool size_done = false;
  while (pos < input_size && !size_done) {
    const uint8_t byte = input[pos++];
    expected_size |= static_cast<uint64_t>(byte & 0x7f) << shift;
    size_done = 0 == (byte & 0x80);
    shift += 7;
    if (shift > 35) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  if (OB_SUCC(ret)) {
    output.clear();
    output.reserve(static_cast<size_t>(expected_size));
    while (OB_SUCC(ret) && pos < input_size && output.size() < expected_size) {
      const uint8_t tag = input[pos++];
      const uint8_t type = tag & 0x03;
      if (0 == type) {
        stage = "literal";
        uint64_t literal_len = tag >> 2;
        if (literal_len < 60) {
          ++literal_len;
        } else {
          const int extra = static_cast<int>(literal_len - 59);
          literal_len = 0;
          if (pos + extra > input_size) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            for (int i = 0; i < extra; ++i) {
              literal_len |= static_cast<uint64_t>(input[pos++]) << (8 * i);
            }
            ++literal_len;
          }
        }
        if (OB_SUCC(ret) && (pos + literal_len > input_size
            || output.size() + literal_len > expected_size)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_SUCC(ret)) {
          output.insert(output.end(), input + pos, input + pos + literal_len);
          pos += literal_len;
        }
      } else {
        stage = "copy";
        uint64_t copy_len = 0;
        uint64_t offset = 0;
        if (1 == type) {
          if (pos >= input_size) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            copy_len = 4 + ((tag >> 2) & 0x07);
            offset = ((tag >> 5) << 8) | input[pos++];
          }
        } else if (2 == type) {
          if (pos + 2 > input_size) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            copy_len = 1 + (tag >> 2);
            offset = input[pos] | (static_cast<uint64_t>(input[pos + 1]) << 8);
            pos += 2;
          }
        } else if (3 == type) {
          if (pos + 4 > input_size) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            copy_len = 1 + (tag >> 2);
            offset = input[pos]
                | (static_cast<uint64_t>(input[pos + 1]) << 8)
                | (static_cast<uint64_t>(input[pos + 2]) << 16)
                | (static_cast<uint64_t>(input[pos + 3]) << 24);
            pos += 4;
          }
        }
        if (OB_SUCC(ret) && (0 == offset || offset > output.size()
            || output.size() + copy_len > expected_size)) {
          ret = OB_INVALID_ARGUMENT;
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < copy_len; ++i) {
          output.push_back(output[output.size() - offset]);
        }
      }
    }
  }
  if (OB_SUCC(ret) && output.size() != expected_size) {
    stage = "size_mismatch";
    ret = OB_INVALID_ARGUMENT;
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("failed to decompress snappy data", K(ret), K(stage),
             K(input_size), K(expected_size),
             K(static_cast<int64_t>(output.size())), K(pos));
  }
  return ret;
}

int gzip_decompress(const uint8_t *input,
                    const int64_t input_size,
                    const int64_t expected_size,
                    std::vector<uint8_t> &output)
{
  int ret = OB_SUCCESS;
  output.resize(static_cast<size_t>(expected_size));
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  stream.next_in = const_cast<Bytef *>(input);
  stream.avail_in = static_cast<uInt>(input_size);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());
  if (Z_OK != inflateInit2(&stream, 15 + 32)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const int rc = inflate(&stream, Z_FINISH);
    if (Z_STREAM_END != rc || stream.total_out != static_cast<uLong>(expected_size)) {
      ret = OB_INVALID_ARGUMENT;
    }
    inflateEnd(&stream);
  }
  return ret;
}

int decompress_page(const int32_t codec,
                    const uint8_t *input,
                    const int64_t input_size,
                    const int64_t expected_size,
                    std::vector<uint8_t> &output)
{
  int ret = OB_SUCCESS;
  if (input_size < 0 || input_size > MAX_PARQUET_PAGE_SIZE
      || expected_size < 0 || expected_size > MAX_PARQUET_PAGE_SIZE) {
    ret = OB_SIZE_OVERFLOW;
  } else if (0 == codec) {
    output.assign(input, input + input_size);
  } else if (1 == codec) {
    ret = snappy_decompress(input, input_size, output);
  } else if (2 == codec) {
    ret = gzip_decompress(input, input_size, expected_size, output);
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret) && output.size() != static_cast<size_t>(expected_size)) {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

uint32_t read_uleb128(const uint8_t *data, const int64_t size, int64_t &pos)
{
  uint32_t value = 0;
  int shift = 0;
  while (pos < size && shift < 32) {
    const uint8_t byte = data[pos++];
    value |= static_cast<uint32_t>(byte & 0x7f) << shift;
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
  }
  return value;
}

int decode_rle_hybrid(const uint8_t *data,
                      const int64_t size,
                      const int64_t bit_width,
                      const int64_t max_values,
                      std::vector<uint32_t> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const int64_t byte_width = (bit_width + 7) / 8;
  while (OB_SUCC(ret) && pos < size && static_cast<int64_t>(values.size()) < max_values) {
    const uint32_t header = read_uleb128(data, size, pos);
    if (0 == (header & 1)) {
      const int64_t run_len = header >> 1;
      uint32_t value = 0;
      if (pos + byte_width > size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        for (int64_t i = 0; i < byte_width; ++i) {
          value |= static_cast<uint32_t>(data[pos++]) << (8 * i);
        }
        for (int64_t i = 0;
             OB_SUCC(ret) && i < run_len && static_cast<int64_t>(values.size()) < max_values;
             ++i) {
          values.push_back(value);
        }
      }
    } else {
      const int64_t groups = header >> 1;
      const int64_t packed_values = groups * 8;
      const int64_t packed_bytes = (packed_values * bit_width + 7) / 8;
      if (pos + packed_bytes > size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        for (int64_t i = 0;
             OB_SUCC(ret) && i < packed_values && static_cast<int64_t>(values.size()) < max_values;
             ++i) {
          const int64_t bit_offset = i * bit_width;
          const int64_t byte_offset = bit_offset / 8;
          const int shift = static_cast<int>(bit_offset % 8);
          uint64_t packed = 0;
          for (int64_t j = 0; j < 8 && pos + byte_offset + j < size; ++j) {
            packed |= static_cast<uint64_t>(data[pos + byte_offset + j]) << (8 * j);
          }
          const uint32_t mask = bit_width >= 32
              ? std::numeric_limits<uint32_t>::max()
              : ((static_cast<uint32_t>(1) << bit_width) - 1);
          values.push_back(static_cast<uint32_t>((packed >> shift) & mask));
        }
        pos += packed_bytes;
      }
    }
  }
  if (OB_SUCC(ret) && static_cast<int64_t>(values.size()) < max_values) {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

int decode_levels(const uint8_t *data,
                  const int64_t size,
                  const int64_t max_level,
                  const int64_t value_count,
                  std::vector<uint16_t> &levels)
{
  int ret = OB_SUCCESS;
  levels.clear();
  if (max_level <= 0) {
    levels.assign(static_cast<size_t>(value_count), 0);
  } else if (size < 4) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const uint32_t encoded_size = read_u32_le(data);
    if (static_cast<int64_t>(encoded_size) + 4 > size) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      int bit_width = 0;
      for (int64_t value = max_level; value > 0; value >>= 1) {
        ++bit_width;
      }
      std::vector<uint32_t> decoded;
      if (OB_FAIL(decode_rle_hybrid(data + 4, encoded_size, bit_width, value_count, decoded))) {
      } else {
        levels.reserve(static_cast<size_t>(value_count));
        for (int64_t i = 0; i < value_count; ++i) {
          levels.push_back(static_cast<uint16_t>(decoded[static_cast<size_t>(i)]));
        }
      }
    }
  }
  return ret;
}

std::string float_to_string(const double value)
{
  std::ostringstream out;
  out.precision(std::numeric_limits<double>::max_digits10);
  out << value;
  return out.str();
}

std::string date_days_to_string(const int32_t days)
{
  int64_t z = static_cast<int64_t>(days) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t year = static_cast<int64_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t day = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02u",
           static_cast<long long>(year), month, day);
  return buffer;
}

std::string datetime_value_to_string(const int64_t value,
                                    const int32_t converted_type)
{
  int64_t seconds = value;
  if (9 == converted_type) {
    seconds /= 1000;
  } else if (10 == converted_type) {
    seconds /= 1000000;
  }
  const int64_t days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
  int64_t day_seconds = seconds - days * 86400;
  if (day_seconds < 0) {
    day_seconds += 86400;
  }
  const int64_t hour = day_seconds / 3600;
  const int64_t minute = (day_seconds % 3600) / 60;
  const int64_t second = day_seconds % 60;
  char buffer[64];
  const std::string date = date_days_to_string(static_cast<int32_t>(days));
  snprintf(buffer, sizeof(buffer), "%s %02lld:%02lld:%02lld",
           date.c_str(), static_cast<long long>(hour),
           static_cast<long long>(minute), static_cast<long long>(second));
  return buffer;
}

int decode_plain_values(const int32_t physical_type,
                        const int32_t converted_type,
                        const uint8_t *data,
                        const int64_t size,
                        const int64_t count,
                        std::vector<std::string> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  values.clear();
  values.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    if (0 == physical_type) {
      const int64_t byte_pos = i / 8;
      if (byte_pos >= size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        values.push_back((data[byte_pos] & (1 << (i % 8))) ? "1" : "0");
      }
    } else if (1 == physical_type) {
      if (pos + 4 > size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const int32_t raw = static_cast<int32_t>(read_u32_le(data + pos));
        pos += 4;
        values.push_back(6 == converted_type
            ? date_days_to_string(raw) : std::to_string(raw));
      }
    } else if (2 == physical_type) {
      if (pos + 8 > size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const int64_t raw = static_cast<int64_t>(read_u64_le(data + pos));
        pos += 8;
        values.push_back((9 == converted_type || 10 == converted_type)
            ? datetime_value_to_string(raw, converted_type) : std::to_string(raw));
      }
    } else if (4 == physical_type) {
      if (pos + 4 > size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const uint32_t bits = read_u32_le(data + pos);
        float raw = 0;
        memcpy(&raw, &bits, sizeof(raw));
        pos += 4;
        values.push_back(float_to_string(raw));
      }
    } else if (5 == physical_type) {
      if (pos + 8 > size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const uint64_t bits = read_u64_le(data + pos);
        double raw = 0;
        memcpy(&raw, &bits, sizeof(raw));
        pos += 8;
        values.push_back(float_to_string(raw));
      }
    } else if (6 == physical_type) {
      if (pos + 4 > size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const uint32_t len = read_u32_le(data + pos);
        pos += 4;
        if (pos + len > size) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          values.emplace_back(reinterpret_cast<const char *>(data + pos), len);
          pos += len;
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

int decode_dictionary_values(const int32_t physical_type,
                             const int32_t converted_type,
                             const uint8_t *data,
                             const int64_t size,
                             const int64_t count,
                             std::vector<std::string> &values)
{
  int ret = OB_SUCCESS;
  if (size <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const int bit_width = data[0];
    std::vector<uint32_t> indices;
    if (OB_FAIL(decode_rle_hybrid(data + 1, size - 1, bit_width, count, indices))) {
    } else {
      std::vector<std::string> dictionary;
      // The caller replaces this helper with a dictionary-aware page decoder.
      values.clear();
      for (size_t i = 0; i < indices.size(); ++i) {
        values.push_back(std::to_string(indices[i]));
      }
    }
  }
  return ret;
}

struct ParquetColumnData
{
  int32_t physical_type_;
  int32_t converted_type_;
  std::vector<ScalarValue> values_;
};

int decode_data_page(const int32_t physical_type,
                     const int32_t converted_type,
                     const int32_t encoding,
                     const int64_t max_definition_level,
                     const int64_t max_repetition_level,
                     const uint8_t *payload,
                     const int64_t payload_size,
                     const int64_t page_value_count,
                     const int64_t null_count,
                     const bool v2,
                     const int64_t definition_size,
                     const int64_t repetition_size,
                     const std::vector<std::string> &dictionary,
                     std::vector<ScalarValue> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  std::vector<uint16_t> definition_levels;
  std::vector<std::string> decoded_values;
  if (payload_size < 0 || payload_size > MAX_PARQUET_PAGE_SIZE
      || page_value_count < 0
      || page_value_count > MAX_PARQUET_ROW_GROUP_VALUES
      || null_count < -1
      || definition_size < 0 || repetition_size < 0
      || definition_size > MAX_PARQUET_PAGE_SIZE
      || repetition_size > MAX_PARQUET_PAGE_SIZE) {
    ret = OB_INVALID_ARGUMENT;
  } else if (max_repetition_level > 0) {
    ret = OB_NOT_SUPPORTED;
  } else if (v2) {
    pos += repetition_size;
    if (pos + definition_size > payload_size) {
      ret = OB_INVALID_ARGUMENT;
    } else if (max_definition_level > 0) {
      int bit_width = 0;
      for (int64_t value = max_definition_level; value > 0; value >>= 1) {
        ++bit_width;
      }
      std::vector<uint32_t> decoded;
      if (OB_FAIL(decode_rle_hybrid(payload + pos, definition_size, bit_width,
                                    page_value_count, decoded))) {
      } else {
        for (size_t i = 0; i < decoded.size(); ++i) {
          definition_levels.push_back(static_cast<uint16_t>(decoded[i]));
        }
      }
      pos += definition_size;
    }
  } else if (max_definition_level > 0) {
    int64_t consumed = 0;
    if (pos + 4 > payload_size) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      consumed = 4 + read_u32_le(payload + pos);
      if (pos + consumed > payload_size
          || OB_FAIL(decode_levels(payload + pos, consumed, max_definition_level,
                                   page_value_count, definition_levels))) {
      } else {
        pos += consumed;
      }
    }
  }
  if (OB_SUCC(ret)) {
    int64_t present_count = page_value_count;
    if (max_definition_level > 0) {
      present_count = 0;
      for (size_t i = 0; i < definition_levels.size(); ++i) {
        if (definition_levels[i] == max_definition_level) {
          ++present_count;
        }
      }
    }
    if (v2 && null_count >= 0) {
      present_count = page_value_count - null_count;
    }
    if (0 == encoding) {
      ret = decode_plain_values(physical_type, converted_type,
                                payload + pos, payload_size - pos,
                                present_count, decoded_values);
    } else if (3 == encoding && 0 == physical_type) {
      int64_t encoded_pos = pos;
      int64_t encoded_size = payload_size - pos;
      if (payload_size - pos >= 4) {
        const int64_t declared_size = read_u32_le(payload + pos);
        if (declared_size >= 0 && declared_size + 4 <= payload_size - pos) {
          encoded_pos += 4;
          encoded_size = declared_size;
        }
      }
      std::vector<uint32_t> decoded;
      if (OB_FAIL(decode_rle_hybrid(payload + encoded_pos, encoded_size,
                                    1, present_count, decoded))) {
      } else {
        for (size_t i = 0; i < decoded.size(); ++i) {
          decoded_values.push_back(0 == decoded[i] ? "0" : "1");
        }
      }
    } else if (2 == encoding || 8 == encoding) {
      if (pos >= payload_size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const int bit_width = payload[pos++];
        std::vector<uint32_t> indices;
        if (OB_FAIL(decode_rle_hybrid(payload + pos, payload_size - pos,
                                      bit_width, present_count, indices))) {
        } else {
          for (size_t i = 0; OB_SUCC(ret) && i < indices.size(); ++i) {
            if (indices[i] >= dictionary.size()) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              decoded_values.push_back(dictionary[indices[i]]);
            }
          }
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
    }
    if (OB_SUCC(ret)) {
      size_t value_pos = 0;
      values.clear();
      values.reserve(static_cast<size_t>(page_value_count));
      for (int64_t i = 0; i < page_value_count; ++i) {
        ScalarValue value;
        const uint16_t level = max_definition_level > 0
            ? definition_levels[static_cast<size_t>(i)] : max_definition_level;
        if (max_definition_level > 0 && level < max_definition_level) {
          value.is_null_ = true;
        } else if (value_pos >= decoded_values.size()) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          value.is_null_ = false;
          value.text_ = std::move(decoded_values[value_pos++]);
        }
        values.push_back(std::move(value));
      }
    }
  }
  return ret;
}

int decode_parquet_column(FILE *file,
                          const ParquetColumnMeta &column,
                          const ParquetSchemaElement &schema,
                          const int64_t row_count,
                          std::vector<ScalarValue> &values)
{
  int ret = OB_SUCCESS;
  const char *stage = "chunk";
  int64_t page_type = -1;
  int64_t decoded_page_count = -1;
  int64_t page_encoding = -1;
  values.clear();
  std::vector<uint8_t> chunk;
  const int64_t start = column.dictionary_page_offset_ > 0
      && column.dictionary_page_offset_ < column.data_page_offset_
      ? column.dictionary_page_offset_ : column.data_page_offset_;
  if (column.total_compressed_size_ <= 0
      || column.total_compressed_size_ > MAX_PARQUET_CHUNK_SIZE
      || column.num_values_ < 0
      || column.num_values_ > MAX_PARQUET_ROW_GROUP_VALUES
      || row_count < 0
      || row_count > MAX_PARQUET_ROW_GROUP_VALUES) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(seek_file(file, start))) {
  } else {
    chunk.resize(static_cast<size_t>(column.total_compressed_size_));
    if (column.total_compressed_size_ !=
        static_cast<int64_t>(fread(chunk.data(), 1, chunk.size(), file))) {
      ret = OB_IO_ERROR;
    }
  }
  int64_t chunk_pos = 0;
  int64_t values_read = 0;
  std::vector<std::string> dictionary;
  const int64_t max_definition_level = 1 == schema.repetition_type_ ? 1 : 0;
  while (OB_SUCC(ret) && chunk_pos < static_cast<int64_t>(chunk.size())
         && values_read < column.num_values_) {
    ThriftCompactReader reader(chunk.data() + chunk_pos,
                               static_cast<int64_t>(chunk.size()) - chunk_pos);
    ThriftValue page_header;
    stage = "page_header";
    if (OB_FAIL(reader.read_struct(page_header))) {
    } else {
      const int64_t header_size = reader.position();
      const ThriftValue *type = page_header.field(1);
      const ThriftValue *uncompressed_size = page_header.field(2);
      const ThriftValue *compressed_size = page_header.field(3);
      page_type = nullptr == type ? -1 : type->integer_;
      if (nullptr == type || nullptr == uncompressed_size || nullptr == compressed_size) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const int64_t payload_offset = chunk_pos + header_size;
        const int64_t payload_size = compressed_size->integer_;
        if (payload_size < 0 || payload_size > MAX_PARQUET_PAGE_SIZE
            || uncompressed_size->integer_ < 0
            || uncompressed_size->integer_ > MAX_PARQUET_PAGE_SIZE
            || payload_offset + payload_size > static_cast<int64_t>(chunk.size())) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          std::vector<uint8_t> payload;
          if (2 == type->integer_) {
            stage = "dictionary_page";
            if (OB_FAIL(decompress_page(column.codec_,
                                        chunk.data() + payload_offset,
                                        payload_size,
                                        uncompressed_size->integer_,
                                        payload))) {
            } else {
              const ThriftValue *dictionary_header = page_header.field(7);
              const ThriftValue *dict_count = nullptr == dictionary_header
                  ? nullptr : dictionary_header->field(1);
              if (nullptr == dict_count
                  || dict_count->integer_ < 0
                  || dict_count->integer_ > MAX_PARQUET_ROW_GROUP_VALUES) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                ret = decode_plain_values(column.physical_type_, schema.converted_type_,
                                          payload.data(), payload.size(),
                                          dict_count->integer_, dictionary);
              }
            }
          } else if (0 == type->integer_) {
            stage = "data_page_v1";
            const ThriftValue *data_header = page_header.field(5);
            const ThriftValue *page_count = nullptr == data_header ? nullptr : data_header->field(1);
            const ThriftValue *encoding = nullptr == data_header ? nullptr : data_header->field(2);
            decoded_page_count = -1;
            if (nullptr == page_count || nullptr == encoding
                || page_count->integer_ < 0
                || page_count->integer_ > MAX_PARQUET_ROW_GROUP_VALUES) {
              ret = OB_INVALID_ARGUMENT;
            } else if (OB_FAIL(decompress_page(column.codec_,
                                               chunk.data() + payload_offset,
                                               payload_size,
                                               uncompressed_size->integer_,
                                               payload))) {
            } else {
              decoded_page_count = page_count->integer_;
              page_encoding = encoding->integer_;
              std::vector<ScalarValue> page_values;
              if (OB_FAIL(decode_data_page(column.physical_type_, schema.converted_type_,
                                           static_cast<int32_t>(encoding->integer_),
                                           max_definition_level, 0,
                                           payload.data(), payload.size(),
                                           page_count->integer_, -1, false, 0, 0,
                                           dictionary, page_values))) {
              } else {
                values.insert(values.end(),
                              std::make_move_iterator(page_values.begin()),
                              std::make_move_iterator(page_values.end()));
                values_read += page_count->integer_;
              }
            }
          } else if (3 == type->integer_) {
            stage = "data_page_v2";
            const ThriftValue *data_header = page_header.field(8);
            const ThriftValue *page_count = nullptr == data_header ? nullptr : data_header->field(1);
            const ThriftValue *null_count = nullptr == data_header ? nullptr : data_header->field(2);
            const ThriftValue *encoding = nullptr == data_header ? nullptr : data_header->field(4);
            const ThriftValue *definition_size = nullptr == data_header
                ? nullptr : data_header->field(5);
            const ThriftValue *repetition_size = nullptr == data_header
                ? nullptr : data_header->field(6);
            const ThriftValue *compressed_flag = nullptr == data_header
                ? nullptr : data_header->field(7);
            if (nullptr == page_count || nullptr == encoding
                || nullptr == definition_size || nullptr == repetition_size
                || page_count->integer_ < 0
                || page_count->integer_ > MAX_PARQUET_ROW_GROUP_VALUES
                || definition_size->integer_ < 0
                || definition_size->integer_ > MAX_PARQUET_PAGE_SIZE
                || repetition_size->integer_ < 0
                || repetition_size->integer_ > MAX_PARQUET_PAGE_SIZE) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              const int64_t level_size =
                  definition_size->integer_ + repetition_size->integer_;
              const int64_t value_compressed_size = payload_size - level_size;
              const int64_t value_uncompressed_size =
                  uncompressed_size->integer_ - level_size;
              if (level_size < 0 || value_compressed_size < 0
                  || value_uncompressed_size < 0 || level_size > payload_size) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                decoded_page_count = page_count->integer_;
                page_encoding = encoding->integer_;
                std::vector<uint8_t> value_payload;
                const uint8_t *compressed_values =
                    chunk.data() + payload_offset + level_size;
                const bool is_compressed =
                    nullptr == compressed_flag || compressed_flag->bool_;
                if (is_compressed && OB_FAIL(decompress_page(
                        column.codec_, compressed_values, value_compressed_size,
                        value_uncompressed_size, value_payload))) {
                } else if (!is_compressed) {
                  if (value_compressed_size != value_uncompressed_size) {
                    ret = OB_INVALID_ARGUMENT;
                  } else {
                    value_payload.assign(compressed_values,
                                         compressed_values + value_compressed_size);
                  }
                }
                if (OB_SUCC(ret)) {
                  payload.reserve(static_cast<size_t>(level_size + value_payload.size()));
                  payload.insert(payload.end(), chunk.data() + payload_offset,
                                 chunk.data() + payload_offset + level_size);
                  payload.insert(payload.end(), value_payload.begin(), value_payload.end());
                  std::vector<ScalarValue> page_values;
                  if (OB_FAIL(decode_data_page(
                          column.physical_type_, schema.converted_type_,
                          static_cast<int32_t>(encoding->integer_),
                          max_definition_level, 0,
                          payload.data(), payload.size(),
                          page_count->integer_,
                          nullptr == null_count ? -1 : null_count->integer_,
                          true, definition_size->integer_,
                          repetition_size->integer_,
                          dictionary, page_values))) {
                  } else {
                    values.insert(values.end(),
                                  std::make_move_iterator(page_values.begin()),
                                  std::make_move_iterator(page_values.end()));
                    values_read += page_count->integer_;
                  }
                }
              }
            }
          }
          chunk_pos = payload_offset + payload_size;
        }
      }
    }
  }
  if (OB_SUCC(ret) && values_read != column.num_values_) {
    ret = OB_INVALID_ARGUMENT;
  }
  if (OB_SUCC(ret) && static_cast<int64_t>(values.size()) < row_count) {
    ret = OB_INVALID_ARGUMENT;
  }
  if (OB_SUCC(ret)) {
    values.resize(static_cast<size_t>(row_count));
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("failed to decode parquet column", K(ret), K(stage),
             K(page_type), K(decoded_page_count), K(page_encoding),
             K(values_read), K(column.num_values_), K(row_count),
             K(static_cast<int64_t>(values.size())), K(column.codec_));
  }
  return ret;
}

class ParquetReader final : public IFileReader
{
public:
  ParquetReader(const std::string &path,
                const TableSchema &schema,
                const std::vector<int64_t> &projection)
    : path_(path), schema_(schema), projection_(projection), file_(NULL),
      current_row_group_(-1)
  {}
  virtual ~ParquetReader() { close_file(file_); }
  virtual int rescan() override
  {
    int ret = OB_SUCCESS;
    close_file(file_);
    meta_ = ParquetFileMeta();
    current_row_group_ = -1;
    row_index_ = 0;
    if (OB_FAIL(open_file(path_, file_))) {
    } else if (OB_FAIL(read_parquet_footer(file_, meta_))) {
    }
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to reset parquet reader", K(ret),
               K(meta_.schema_.size()), K(meta_.row_groups_.size()));
    }
    return ret;
  }
  virtual int get_next_row(std::vector<ScalarValue> &row) override
  {
    int ret = OB_SUCCESS;
    row.clear();
    row.resize(schema_.columns_.size());
    while (OB_SUCC(ret)) {
      if (current_row_group_ < 0
          || row_index_ >= meta_.row_groups_[static_cast<size_t>(current_row_group_)].num_rows_) {
        if (OB_FAIL(load_next_row_group())) {
        }
      }
      if (OB_SUCC(ret)) {
        const ParquetRowGroup &group =
            meta_.row_groups_[static_cast<size_t>(current_row_group_)];
        if (row_index_ < group.num_rows_) {
          for (size_t i = 0; i < projection_.size(); ++i) {
            const int64_t column_idx = projection_[i];
            const ParquetColumnData &column = row_group_columns_[i];
            row[static_cast<size_t>(column_idx)] =
                column.values_[static_cast<size_t>(row_index_)];
          }
          ++row_index_;
          break;
        }
      }
    }
    return ret;
  }

private:
  int build_schema_projection()
  {
    int ret = OB_SUCCESS;
    leaf_schema_indexes_.clear();
    for (size_t i = 1; i < meta_.schema_.size(); ++i) {
      const ParquetSchemaElement &element = meta_.schema_[i];
      if (element.physical_type_ >= 0 && element.num_children_ == 0) {
        leaf_schema_indexes_.push_back(static_cast<int64_t>(i));
      }
    }
    if (leaf_schema_indexes_.size() != schema_.columns_.size()) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("nested parquet schemas are not supported", K(ret));
    }
    return ret;
  }

  int load_next_row_group()
  {
    int ret = OB_SUCCESS;
    row_group_columns_.clear();
    row_index_ = 0;
    ++current_row_group_;
    if (current_row_group_ >= static_cast<int64_t>(meta_.row_groups_.size())) {
      ret = OB_ITER_END;
    } else if (OB_FAIL(build_schema_projection())) {
    } else {
      const ParquetRowGroup &group =
          meta_.row_groups_[static_cast<size_t>(current_row_group_)];
      row_group_columns_.resize(projection_.size());
      for (size_t i = 0; OB_SUCC(ret) && i < projection_.size(); ++i) {
        const int64_t schema_idx = projection_[i];
        if (schema_idx < 0 || schema_idx >= static_cast<int64_t>(schema_.columns_.size())) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          const ParquetSchemaElement &schema =
              meta_.schema_[static_cast<size_t>(leaf_schema_indexes_[schema_idx])];
          const ParquetColumnMeta *target_column = nullptr;
          for (size_t j = 0; j < group.columns_.size(); ++j) {
            if (!group.columns_[j].path_.empty()
                && (group.columns_[j].path_.back() == schema.name_
                    || (schema.name_.empty() && j == static_cast<size_t>(schema_idx)))) {
              if (static_cast<int64_t>(j) == schema_idx || group.columns_.size() == schema_.columns_.size()) {
                target_column = &group.columns_[j];
                break;
              }
            }
          }
          if (nullptr == target_column) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            row_group_columns_[i].physical_type_ = target_column->physical_type_;
            row_group_columns_[i].converted_type_ = schema.converted_type_;
            if (OB_FAIL(decode_parquet_column(file_, *target_column, schema,
                                              group.num_rows_,
                                              row_group_columns_[i].values_))) {
              LOG_WARN("failed to decode projected parquet column", K(ret), K(i),
                       K(schema_idx), K(schema.name_.c_str()),
                       K(target_column->physical_type_),
                       K(target_column->num_values_), K(group.num_rows_));
            }
          }
        }
      }
    }
    return ret;
  }

  std::string path_;
  TableSchema schema_;
  std::vector<int64_t> projection_;
  FILE *file_;
  ParquetFileMeta meta_;
  std::vector<int64_t> leaf_schema_indexes_;
  int64_t current_row_group_;
  int64_t row_index_;
  std::vector<ParquetColumnData> row_group_columns_;
};

bool wildcard_match(const std::string &pattern, const std::string &text)
{
  size_t p = 0;
  size_t t = 0;
  size_t star = std::string::npos;
  size_t retry = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      retry = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++retry;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

} // namespace

int get_file_stat(const std::string &path, FileStat &stat)
{
  int ret = OB_SUCCESS;
  std::error_code error;
  const std::filesystem::path fs_path(path);
  if (!std::filesystem::is_regular_file(fs_path, error)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("file sql path is not a regular file", K(ret), K(path.c_str()),
             K(error.message().c_str()));
  } else {
    stat.size_ = static_cast<int64_t>(std::filesystem::file_size(fs_path, error));
    if (error) {
      ret = OB_IO_ERROR;
    } else {
      stat.modify_time_ = static_cast<int64_t>(
          std::filesystem::last_write_time(fs_path, error).time_since_epoch().count());
      if (error) {
        ret = OB_IO_ERROR;
      }
    }
  }
  return ret;
}

int infer_table_schema(const std::string &path,
                       const FileFormat format,
                       const FileOptions &options,
                       TableSchema &schema)
{
  int ret = OB_SUCCESS;
  schema.columns_.clear();
  schema.row_count_ = 0;
  if (FileFormat::GLOB == format) {
    ColumnSchema column;
    column.name_ = "file";
    column.type_ = ColumnType::VARCHAR;
    column.nullable_ = false;
    schema.columns_.push_back(column);
    std::vector<std::string> files;
    if (OB_FAIL(list_glob_files(path, files))) {
    } else {
      schema.row_count_ = static_cast<int64_t>(files.size());
    }
  } else if (is_describe_format(format)) {
    TableSchema source_schema;
    if (OB_FAIL(infer_table_schema(path,
                                   get_describe_base_format(format),
                                   options,
                                   source_schema))) {
    } else {
      build_describe_schema(source_schema, schema);
    }
  } else if (OB_FAIL(get_file_stat(path, schema.stat_))) {
  } else if (FileFormat::CSV == format) {
    ret = infer_csv_schema(path, options, schema);
  } else if (FileFormat::JSON == format) {
    ret = infer_json_schema(path, schema);
  } else if (FileFormat::PARQUET == format) {
    FILE *file = NULL;
    if (OB_FAIL(open_file(path, file))) {
    } else {
      ret = infer_parquet_schema(file, schema);
      close_file(file);
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("file schema inference for this format is not supported yet", K(ret), K(path.c_str()));
  }
  return ret;
}

int create_file_reader(const std::string &path,
                       const FileFormat format,
                       const FileOptions &options,
                       const TableSchema &schema,
                       const std::vector<int64_t> &projection,
                       std::unique_ptr<IFileReader> &reader)
{
  int ret = OB_SUCCESS;
  if (is_describe_format(format)) {
    TableSchema source_schema;
    if (OB_FAIL(infer_table_schema(path,
                                   get_describe_base_format(format),
                                   options,
                                   source_schema))) {
    } else {
      reader.reset(new (std::nothrow) DescribeReader(source_schema));
    }
  } else if (FileFormat::CSV == format) {
    reader.reset(new (std::nothrow) CsvReader(path, options, schema));
  } else if (FileFormat::JSON == format) {
    reader.reset(new (std::nothrow) JsonReader(path, schema));
  } else if (FileFormat::PARQUET == format) {
    reader.reset(new (std::nothrow) ParquetReader(path, schema, projection));
  } else if (FileFormat::GLOB == format) {
    reader.reset(new (std::nothrow) GlobReader(path));
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret) && nullptr == reader) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_SUCC(ret) && OB_FAIL(reader->rescan())) {
  }
  return ret;
}

int list_glob_files(const std::string &pattern, std::vector<std::string> &files)
{
  int ret = OB_SUCCESS;
  files.clear();
  std::error_code error;
  std::filesystem::path pattern_path(pattern);
  const std::string filename_pattern = pattern_path.filename().string();
  std::filesystem::path directory = pattern_path.parent_path();
  if (directory.empty()) {
    directory = ".";
  }
  const std::filesystem::path canonical_directory =
      std::filesystem::weakly_canonical(directory, error);
  if (error || !std::filesystem::is_directory(canonical_directory, error)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("glob directory does not exist", K(ret), K(pattern.c_str()),
             K(error.message().c_str()));
  } else {
    for (std::filesystem::directory_iterator it(canonical_directory, error), end;
         OB_SUCC(ret) && !error && it != end;
         it.increment(error)) {
      std::error_code type_error;
      if (!it->is_regular_file(type_error)) {
        continue;
      }
      const std::string name = it->path().filename().string();
      if (wildcard_match(filename_pattern, name)) {
        files.push_back(it->path().generic_string());
      }
    }
    if (error) {
      ret = OB_IO_ERROR;
    } else {
      std::sort(files.begin(), files.end());
    }
  }
  return ret;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
