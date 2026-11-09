#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace mvd {

namespace fs = std::filesystem;

constexpr uint32_t kRowMagic = 0x5256444dU;
constexpr uint32_t kCommitMagic = 0x434d564dU;
constexpr uint32_t kCatalogMagic = 0x4341564dU;
constexpr uint16_t kFormatVersion = 1;
constexpr size_t kDefaultCacheBytes = 192ULL * 1024ULL * 1024ULL;

size_t cacheLimitBytes() {
  const char *configured = std::getenv("MVD_CACHE_LIMIT_MB");
  if (configured == nullptr) {
    return kDefaultCacheBytes;
  }
  char *end = nullptr;
  errno = 0;
  unsigned long long megabytes = std::strtoull(configured, &end, 10);
  if (errno != 0 || end == configured || *end != '\0' || megabytes == 0) {
    return kDefaultCacheBytes;
  }
  if (megabytes > (std::numeric_limits<size_t>::max() / (1024ULL * 1024ULL))) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(megabytes) * 1024ULL * 1024ULL;
}

class DBError : public std::runtime_error {
 public:
  DBError(std::string code, std::string message)
      : std::runtime_error(std::move(message)), code_(std::move(code)) {}

  const std::string &code() const noexcept { return code_; }

 private:
  std::string code_;
};

[[noreturn]] void fail(const char *code, const std::string &message) {
  throw DBError(code, message);
}

std::string lower(std::string value) {
  for (char &c : value) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return value;
}

uint32_t crc32(const uint8_t *data, size_t size) {
  static constexpr auto table = [] {
    std::array<uint32_t, 256> values{};
    for (uint32_t i = 0; i < values.size(); ++i) {
      uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
      }
      values[i] = value;
    }
    return values;
  }();
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < size; ++i) {
    crc = (crc >> 8U) ^ table[(crc ^ data[i]) & 0xffU];
  }
  return ~crc;
}

uint32_t crc32(const std::vector<uint8_t> &data) {
  return crc32(data.data(), data.size());
}

template <typename T>
void appendScalar(std::vector<uint8_t> &out, T value) {
  using U = std::make_unsigned_t<T>;
  U bits = static_cast<U>(value);
  for (size_t i = 0; i < sizeof(T); ++i) {
    out.push_back(static_cast<uint8_t>((bits >> (i * 8U)) & 0xffU));
  }
}

void appendBytes(std::vector<uint8_t> &out, const void *data, size_t size) {
  if (size == 0) {
    return;
  }
  const auto *ptr = static_cast<const uint8_t *>(data);
  out.insert(out.end(), ptr, ptr + size);
}

void appendString(std::vector<uint8_t> &out, const std::string &value) {
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    fail("VALUE_TOO_LARGE", "string value is too large");
  }
  appendScalar<uint32_t>(out, static_cast<uint32_t>(value.size()));
  appendBytes(out, value.data(), value.size());
}

template <typename T>
T readScalar(const std::vector<uint8_t> &data, size_t &pos) {
  if (pos + sizeof(T) > data.size()) {
    fail("CORRUPT_DATA", "truncated binary value");
  }
  using U = std::make_unsigned_t<T>;
  U value = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<U>(data[pos++]) << (i * 8U);
  }
  return static_cast<T>(value);
}

std::string readString(const std::vector<uint8_t> &data, size_t &pos) {
  uint32_t size = readScalar<uint32_t>(data, pos);
  if (pos + size > data.size()) {
    fail("CORRUPT_DATA", "truncated string value");
  }
  std::string value(reinterpret_cast<const char *>(data.data() + pos), size);
  pos += size;
  return value;
}

bool isFiniteFloat32(double value) {
  if (!std::isfinite(value)) {
    return false;
  }
  const float narrowed = static_cast<float>(value);
  return std::isfinite(narrowed);
}

size_t countCodepointsStrict(std::string_view text) {
  size_t i = 0;
  size_t count = 0;
  while (i < text.size()) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    uint32_t cp = 0;
    size_t extra = 0;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xe0U) == 0xc0U) {
      cp = c & 0x1fU;
      extra = 1;
      if (cp == 0) {
        fail("INVALID_UTF8", "overlong UTF-8 sequence");
      }
    } else if ((c & 0xf0U) == 0xe0U) {
      cp = c & 0x0fU;
      extra = 2;
    } else if ((c & 0xf8U) == 0xf0U) {
      cp = c & 0x07U;
      extra = 3;
    } else {
      fail("INVALID_UTF8", "invalid UTF-8 leading byte");
    }
    if (i + extra >= text.size()) {
      fail("INVALID_UTF8", "truncated UTF-8 sequence");
    }
    for (size_t j = 1; j <= extra; ++j) {
      unsigned char next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xc0U) != 0x80U) {
        fail("INVALID_UTF8", "invalid UTF-8 continuation byte");
      }
      cp = (cp << 6U) | (next & 0x3fU);
    }
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) {
      fail("INVALID_UTF8", "invalid Unicode code point");
    }
    i += extra + 1;
    ++count;
  }
  return count;
}

enum class ColumnType : uint8_t { BigInt = 1, VarChar = 2, Vector = 3 };

struct Column {
  std::string name;
  ColumnType type = ColumnType::BigInt;
  uint32_t length = 0;
};

struct TableDef {
  uint32_t oid = 0;
  std::string name;
  std::vector<Column> columns;
  size_t primary_key = 0;
  size_t vector_column = std::numeric_limits<size_t>::max();
  bool vector_index = false;

  size_t columnIndex(std::string_view requested) const {
    const std::string wanted = lower(std::string(requested));
    for (size_t i = 0; i < columns.size(); ++i) {
      if (lower(columns[i].name) == wanted) {
        return i;
      }
    }
    fail("UNKNOWN_COLUMN", "unknown column '" + std::string(requested) + "'");
  }
};

struct Value {
  int64_t integer = 0;
  std::string text;
  std::vector<float> vector;
};

using Row = std::vector<Value>;

size_t serializedRowSize(const Row &row) {
  size_t size = 4;
  for (const Value &value : row) {
    size += 16;
    size += value.text.size();
    size += value.vector.size() * sizeof(float);
  }
  return size;
}

size_t cachedRowBytes(const Row &row) {
  size_t size = sizeof(Row) + row.capacity() * sizeof(Value);
  const size_t inline_text_capacity = std::string{}.capacity();
  for (const Value &value : row) {
    if (value.text.capacity() > inline_text_capacity) {
      size += value.text.capacity() + 1;
    }
    size += value.vector.capacity() * sizeof(float);
  }
  return size;
}

std::vector<uint8_t> serializeRow(const Row &row) {
  if (row.size() > std::numeric_limits<uint32_t>::max()) {
    fail("VALUE_TOO_LARGE", "row has too many columns");
  }
  std::vector<uint8_t> out;
  out.reserve(serializedRowSize(row));
  appendScalar<uint32_t>(out, static_cast<uint32_t>(row.size()));
  for (const Value &value : row) {
    appendScalar<int64_t>(out, value.integer);
    appendString(out, value.text);
    appendScalar<uint32_t>(out, static_cast<uint32_t>(value.vector.size()));
    if constexpr (std::endian::native == std::endian::little) {
      appendBytes(out, value.vector.data(), value.vector.size() * sizeof(float));
    } else {
      for (float component : value.vector) {
        appendScalar<uint32_t>(out, std::bit_cast<uint32_t>(component));
      }
    }
  }
  return out;
}

Row deserializeRow(const std::vector<uint8_t> &data) {
  size_t pos = 0;
  uint32_t count = readScalar<uint32_t>(data, pos);
  if (count > 65535) {
    fail("CORRUPT_DATA", "row column count is invalid");
  }
  Row row;
  row.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Value value;
    value.integer = readScalar<int64_t>(data, pos);
    value.text = readString(data, pos);
    uint32_t vector_size = readScalar<uint32_t>(data, pos);
    if (vector_size > (1U << 20U)) {
      fail("CORRUPT_DATA", "vector dimension is invalid");
    }
    if (vector_size > (data.size() - pos) / sizeof(float)) {
      fail("CORRUPT_DATA", "truncated vector value");
    }
    if constexpr (std::endian::native == std::endian::little) {
      value.vector.resize(vector_size);
      if (vector_size != 0) {
        std::memcpy(value.vector.data(), data.data() + pos,
                    vector_size * sizeof(float));
      }
      pos += vector_size * sizeof(float);
    } else {
      value.vector.reserve(vector_size);
      for (uint32_t j = 0; j < vector_size; ++j) {
        value.vector.push_back(
            std::bit_cast<float>(readScalar<uint32_t>(data, pos)));
      }
    }
    row.push_back(std::move(value));
  }
  if (pos != data.size()) {
    fail("CORRUPT_DATA", "trailing bytes in serialized row");
  }
  return row;
}

std::string formatFloat(float value, int precision = 9) {
  if (value == 0.0F) {
    return "0";
  }
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(precision) << value;
  std::string result = out.str();
  if (result.find('.') != std::string::npos && result.find('e') == std::string::npos &&
      result.find('E') == std::string::npos) {
    while (!result.empty() && result.back() == '0') {
      result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
      result.pop_back();
    }
  }
  return result.empty() || result == "-0" ? "0" : result;
}

std::string formatVector(const std::vector<float> &vector) {
  std::string out = "[";
  for (size_t i = 0; i < vector.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out += formatFloat(vector[i]);
  }
  out.push_back(']');
  return out;
}

std::string valueToString(const Value &value, ColumnType type) {
  switch (type) {
    case ColumnType::BigInt:
      return std::to_string(value.integer);
    case ColumnType::VarChar:
      return value.text;
    case ColumnType::Vector:
      return formatVector(value.vector);
  }
  return {};
}

enum class TokenKind { Identifier, Number, String, Symbol, End };

struct Token {
  TokenKind kind = TokenKind::End;
  std::string text;
  size_t position = 0;
};

bool tokenIsKeyword(const Token &token, std::string_view keyword) {
  return token.kind == TokenKind::Identifier &&
         lower(token.text) == lower(std::string(keyword));
}

class Lexer {
 public:
  explicit Lexer(std::string_view input) : input_(input) {}

  std::vector<Token> tokenize() {
    std::vector<Token> tokens;
    while (true) {
      skipSpaceAndComments();
      if (pos_ >= input_.size()) {
        tokens.push_back({TokenKind::End, "", pos_});
        return tokens;
      }
      const size_t start = pos_;
      const unsigned char c = static_cast<unsigned char>(input_[pos_]);
      if (std::isalpha(c) || c == '_' || c == '$') {
        ++pos_;
        while (pos_ < input_.size()) {
          unsigned char n = static_cast<unsigned char>(input_[pos_]);
          if (!(std::isalnum(n) || n == '_' || n == '$')) {
            break;
          }
          ++pos_;
        }
        tokens.push_back({TokenKind::Identifier,
                          std::string(input_.substr(start, pos_ - start)), start});
        continue;
      }
      if (std::isdigit(c) ||
          (c == '+' && pos_ + 1 < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[pos_ + 1]))) ||
          (c == '-' && pos_ + 1 < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[pos_ + 1])))) {
        ++pos_;
        while (pos_ < input_.size()) {
          unsigned char n = static_cast<unsigned char>(input_[pos_]);
          if (!(std::isdigit(n) || n == '.' || n == 'e' || n == 'E' ||
                n == '+' || n == '-')) {
            break;
          }
          ++pos_;
        }
        tokens.push_back({TokenKind::Number,
                          std::string(input_.substr(start, pos_ - start)), start});
        continue;
      }
      if (c == '\'' || c == '"') {
        const char quote = static_cast<char>(c);
        ++pos_;
        std::string value;
        bool closed = false;
        while (pos_ < input_.size()) {
          char n = input_[pos_++];
          if (n == quote) {
            if (pos_ < input_.size() && input_[pos_] == quote) {
              value.push_back(quote);
              ++pos_;
              continue;
            }
            closed = true;
            break;
          }
          if (n == '\\' && pos_ < input_.size()) {
            char escaped = input_[pos_++];
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
            value.push_back(n);
          }
        }
        if (!closed) {
          fail("SQL_SYNTAX", "unterminated string literal");
        }
        tokens.push_back({TokenKind::String, std::move(value), start});
        continue;
      }
      if (pos_ + 1 < input_.size()) {
        std::string_view two = input_.substr(pos_, 2);
        if (two == "<=" || two == ">=" || two == "!=" || two == "<>") {
          pos_ += 2;
          tokens.push_back({TokenKind::Symbol, std::string(two), start});
          continue;
        }
      }
      ++pos_;
      tokens.push_back({TokenKind::Symbol, std::string(1, static_cast<char>(c)),
                        start});
    }
  }

 private:
  void skipSpaceAndComments() {
    while (pos_ < input_.size()) {
      unsigned char c = static_cast<unsigned char>(input_[pos_]);
      if (std::isspace(c)) {
        ++pos_;
        continue;
      }
      if (c == '-' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '-') {
        pos_ += 2;
        while (pos_ < input_.size() && input_[pos_] != '\n') {
          ++pos_;
        }
        continue;
      }
      if (c == '#') {
        ++pos_;
        while (pos_ < input_.size() && input_[pos_] != '\n') {
          ++pos_;
        }
        continue;
      }
      if (c == '/' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < input_.size() &&
               !(input_[pos_] == '*' && input_[pos_ + 1] == '/')) {
          ++pos_;
        }
        if (pos_ + 1 >= input_.size()) {
          fail("SQL_SYNTAX", "unterminated block comment");
        }
        pos_ += 2;
        continue;
      }
      break;
    }
  }

  std::string_view input_;
  size_t pos_ = 0;
};

enum class CompareOp { Eq, Lt, Le, Gt, Ge };

struct Condition {
  size_t column = 0;
  CompareOp op = CompareOp::Eq;
  Value value;
};

struct SelectItem {
  bool star = false;
  bool distance = false;
  bool hidden = false;
  size_t column = 0;
  std::string source_name;
  std::vector<float> probe;
  std::string output_name;
};

struct OrderKey {
  bool distance = false;
  std::string source_name;
  std::vector<float> probe;
  std::string reference;
  bool descending = false;
};

struct SelectStatement {
  std::string table;
  std::vector<SelectItem> items;
  std::vector<Condition> conditions;
  std::vector<OrderKey> order;
  int64_t limit = -1;
  int64_t offset = 0;
};

struct InsertStatement {
  std::string table;
  std::vector<std::string> columns;
  std::vector<Row> rows;
};

struct Assignment {
  size_t column = 0;
  Value value;
};

struct UpdateStatement {
  std::string table;
  std::vector<Assignment> assignments;
  std::vector<Condition> conditions;
};

struct DeleteStatement {
  std::string table;
  std::vector<Condition> conditions;
};

struct CreateTableStatement {
  TableDef definition;
};

struct CreateIndexStatement {
  std::string table;
  size_t column = 0;
};

struct SetModeStatement {
  bool ann = false;
};

struct EmptyStatement {};

using Statement =
    std::variant<SelectStatement, InsertStatement, UpdateStatement,
                 DeleteStatement, CreateTableStatement, CreateIndexStatement,
                 SetModeStatement, EmptyStatement>;

class Parser {
 public:
  Parser(std::string_view sql,
         const std::unordered_map<std::string, TableDef> &tables)
      : tokens_(Lexer(sql).tokenize()), tables_(tables) {}

  Statement parse() {
    if (acceptKeyword("select")) {
      return parseSelect();
    }
    if (acceptKeyword("insert")) {
      return parseInsert();
    }
    if (acceptKeyword("update")) {
      return parseUpdate();
    }
    if (acceptKeyword("delete")) {
      return parseDelete();
    }
    if (acceptKeyword("create")) {
      if (acceptKeyword("table")) {
        return parseCreateTable();
      }
      if (acceptKeyword("vector")) {
        expectKeyword("index");
        return parseCreateIndex();
      }
      fail("UNSUPPORTED", "only CREATE TABLE and CREATE VECTOR INDEX are supported");
    }
    if (acceptKeyword("begin") || acceptKeyword("start")) {
      while (acceptKeyword("work") || acceptKeyword("transaction")) {
      }
      expectEnd();
      return EmptyStatement{};
    }
    if (acceptKeyword("commit") || acceptKeyword("end")) {
      acceptKeyword("work");
      expectEnd();
      return EmptyStatement{};
    }
    if (acceptKeyword("rollback")) {
      acceptKeyword("work");
      expectEnd();
      return EmptyStatement{};
    }
    if (acceptKeyword("set")) {
      return parseSetMode();
    }
    fail("SQL_SYNTAX", "unsupported or invalid statement");
  }

  std::string firstKeyword() const {
    return tokens_.empty() ? "" : lower(tokens_.front().text);
  }

 private:
  const Token &peek(size_t ahead = 0) const {
    size_t index = std::min(pos_ + ahead, tokens_.size() - 1);
    return tokens_[index];
  }

  bool consumeSymbol(std::string_view symbol) {
    if (peek().kind == TokenKind::Symbol && peek().text == symbol) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expectSymbol(std::string_view symbol) {
    if (!consumeSymbol(symbol)) {
      fail("SQL_SYNTAX", "expected '" + std::string(symbol) + "'");
    }
  }

  bool acceptKeyword(std::string_view keyword) {
    if (tokenIsKeyword(peek(), keyword)) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expectKeyword(std::string_view keyword) {
    if (!acceptKeyword(keyword)) {
      fail("SQL_SYNTAX", "expected keyword " + std::string(keyword));
    }
  }

  std::string expectIdentifier() {
    if (peek().kind != TokenKind::Identifier) {
      fail("SQL_SYNTAX", "expected identifier");
    }
    return tokens_[pos_++].text;
  }

  void expectEnd() {
    if (peek().kind != TokenKind::End) {
      fail("UNSUPPORTED", "unexpected token '" + peek().text + "'");
    }
  }

  const TableDef &requireTable(std::string_view name) const {
    auto found = tables_.find(lower(std::string(name)));
    if (found == tables_.end()) {
      fail("UNKNOWN_TABLE", "unknown table '" + std::string(name) + "'");
    }
    return found->second;
  }

  int64_t parseNonNegativeInteger() {
    if (peek().kind != TokenKind::Number ||
        peek().text.find_first_of(".eE") != std::string::npos) {
      fail("SQL_SYNTAX", "expected non-negative integer");
    }
    std::string text = peek().text;
    if (!text.empty() && text.front() == '+') {
      text.erase(text.begin());
    }
    int64_t value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size() || value < 0) {
      fail("VALUE_OUT_OF_RANGE", "integer limit is out of range");
    }
    ++pos_;
    return value;
  }

  int64_t parseBigIntToken() {
    if (peek().kind != TokenKind::Number ||
        peek().text.find_first_of(".eE") != std::string::npos) {
      fail("TYPE_MISMATCH", "BIGINT requires an integer literal");
    }
    std::string text = peek().text;
    if (!text.empty() && text.front() == '+') {
      text.erase(text.begin());
    }
    int64_t value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
      fail("VALUE_OUT_OF_RANGE", "BIGINT value is out of range");
    }
    ++pos_;
    return value;
  }

  double parseDoubleToken() {
    if (peek().kind != TokenKind::Number) {
      fail("TYPE_MISMATCH", "vector component must be numeric");
    }
    const std::string &text = peek().text;
    const char *begin = text.data();
    const char *end = begin + text.size();
    if (begin != end && *begin == '+') {
      ++begin;
    }
    double value = 0;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end ||
        !isFiniteFloat32(value)) {
      fail("VALUE_OUT_OF_RANGE", "vector component is not a finite float32");
    }
    ++pos_;
    return value;
  }

  std::vector<float> parseVectorBody() {
    expectSymbol("[");
    std::vector<float> result;
    if (consumeSymbol("]")) {
      return result;
    }
    while (true) {
      result.push_back(static_cast<float>(parseDoubleToken()));
      if (consumeSymbol("]")) {
        return result;
      }
      expectSymbol(",");
    }
  }

  std::vector<float> parseVectorLiteralText(const std::string &text) {
    Parser nested(text, tables_);
    std::vector<float> value = nested.parseVectorBody();
    nested.expectEnd();
    return value;
  }

  Value parseTypedValue(ColumnType type, uint32_t length) {
    Value value;
    if (tokenIsKeyword(peek(), "null")) {
      fail("UNSUPPORTED", "NULL values are not supported");
    }
    if (type == ColumnType::BigInt) {
      value.integer = parseBigIntToken();
      return value;
    }
    if (type == ColumnType::VarChar) {
      if (peek().kind != TokenKind::String &&
          peek().kind != TokenKind::Identifier &&
          peek().kind != TokenKind::Number) {
        fail("TYPE_MISMATCH", "VARCHAR requires a string literal");
      }
      value.text = tokens_[pos_++].text;
      if (countCodepointsStrict(value.text) > length) {
        fail("VALUE_TOO_LONG", "VARCHAR value exceeds declared length");
      }
      return value;
    }
    if (peek().kind == TokenKind::Symbol && peek().text == "[") {
      value.vector = parseVectorBody();
    } else if (peek().kind == TokenKind::String) {
      value.vector = parseVectorLiteralText(tokens_[pos_++].text);
    } else {
      fail("TYPE_MISMATCH", "VECTOR requires a bracketed numeric array");
    }
    if (value.vector.size() != length) {
      fail("VECTOR_DIMENSION_MISMATCH", "vector dimension does not match schema");
    }
    return value;
  }

  std::vector<Condition> parseConditions(const TableDef &table,
                                         bool allow_empty) {
    std::vector<Condition> conditions;
    if (!acceptKeyword("where")) {
      if (!allow_empty) {
        fail("SQL_SYNTAX", "expected WHERE");
      }
      return conditions;
    }
    while (true) {
      Condition condition;
      condition.column = table.columnIndex(expectIdentifier());
      const Column &column = table.columns[condition.column];
      if (column.type == ColumnType::Vector) {
        fail("UNSUPPORTED", "VECTOR columns cannot be used in WHERE comparisons");
      }
      if (consumeSymbol("=")) {
        condition.op = CompareOp::Eq;
      } else if (consumeSymbol("<=")) {
        condition.op = CompareOp::Le;
      } else if (consumeSymbol(">=")) {
        condition.op = CompareOp::Ge;
      } else if (consumeSymbol("<")) {
        condition.op = CompareOp::Lt;
      } else if (consumeSymbol(">")) {
        condition.op = CompareOp::Gt;
      } else {
        fail("UNSUPPORTED", "only =, <, <=, >, >=, and AND are supported");
      }
      // Predicate literals need not fit the target column's storage width.
      condition.value = parseTypedValue(
          column.type, column.type == ColumnType::VarChar
                           ? std::numeric_limits<uint32_t>::max()
                           : column.length);
      conditions.push_back(std::move(condition));
      if (!acceptKeyword("and")) {
        break;
      }
    }
    return conditions;
  }

  SelectStatement parseSelect() {
    SelectStatement statement;
    if (tokenIsKeyword(peek(), "distinct")) {
      fail("UNSUPPORTED", "DISTINCT is not supported");
    }
    while (true) {
      SelectItem item;
      if (consumeSymbol("*")) {
        item.star = true;
      } else if (tokenIsKeyword(peek(), "l2_distance")) {
        ++pos_;
        expectSymbol("(");
        item.distance = true;
        item.source_name = expectIdentifier();
        expectSymbol(",");
        if (peek().kind == TokenKind::Symbol && peek().text == "[") {
          item.probe = parseVectorBody();
        } else if (peek().kind == TokenKind::String) {
          item.probe = parseVectorLiteralText(tokens_[pos_++].text);
        } else {
          fail("TYPE_MISMATCH", "L2_DISTANCE requires a vector literal");
        }
        expectSymbol(")");
        item.output_name = item.source_name;
      } else {
        item.source_name = expectIdentifier();
        item.output_name = item.source_name;
      }
      if (acceptKeyword("as")) {
        item.output_name = expectIdentifier();
      }
      statement.items.push_back(std::move(item));
      if (!consumeSymbol(",")) {
        break;
      }
    }
    expectKeyword("from");
    statement.table = expectIdentifier();
    const TableDef &table = requireTable(statement.table);
    for (SelectItem &item : statement.items) {
      if (item.distance) {
        item.column = table.columnIndex(item.source_name);
        if (table.columns[item.column].type != ColumnType::Vector) {
          fail("TYPE_MISMATCH", "L2_DISTANCE requires a VECTOR column");
        }
        if (item.probe.size() != table.columns[item.column].length) {
          fail("VECTOR_DIMENSION_MISMATCH",
               "query vector dimension does not match schema");
        }
      } else if (!item.star) {
        item.column = table.columnIndex(item.source_name);
      }
    }
    statement.conditions = parseConditions(table, true);
    if (acceptKeyword("order")) {
      expectKeyword("by");
      while (true) {
        OrderKey key;
        if (tokenIsKeyword(peek(), "l2_distance")) {
          ++pos_;
          expectSymbol("(");
          key.distance = true;
          key.source_name = expectIdentifier();
          expectSymbol(",");
          if (peek().kind == TokenKind::Symbol && peek().text == "[") {
            key.probe = parseVectorBody();
          } else if (peek().kind == TokenKind::String) {
            key.probe = parseVectorLiteralText(tokens_[pos_++].text);
          } else {
            fail("TYPE_MISMATCH",
                 "ORDER BY L2_DISTANCE requires a vector literal");
          }
          expectSymbol(")");
          key.reference = key.source_name;
        } else {
          key.reference = expectIdentifier();
        }
        if (acceptKeyword("desc")) {
          key.descending = true;
        } else {
          acceptKeyword("asc");
        }
        statement.order.push_back(std::move(key));
        if (!consumeSymbol(",")) {
          break;
        }
      }
    }
    if (acceptKeyword("limit")) {
      statement.limit = parseNonNegativeInteger();
      if (consumeSymbol(",")) {
        statement.offset = statement.limit;
        statement.limit = parseNonNegativeInteger();
      } else if (acceptKeyword("offset")) {
        statement.offset = parseNonNegativeInteger();
      }
    }
    for (const OrderKey &key : statement.order) {
      if (!key.distance) {
        continue;
      }
      const size_t column = table.columnIndex(key.source_name);
      if (table.columns[column].type != ColumnType::Vector) {
        fail("TYPE_MISMATCH", "L2_DISTANCE requires a VECTOR column");
      }
      if (key.probe.size() != table.columns[column].length) {
        fail("VECTOR_DIMENSION_MISMATCH", "query vector dimension does not match schema");
      }
      const bool selected = std::any_of(
          statement.items.begin(), statement.items.end(), [&](const SelectItem &item) {
            return item.distance && item.column == column && item.probe == key.probe;
          });
      if (!selected) {
        SelectItem item;
        item.distance = true;
        item.hidden = true;
        item.column = column;
        item.source_name = key.source_name;
        item.probe = key.probe;
        statement.items.push_back(std::move(item));
      }
    }
    expectEnd();
    return statement;
  }

  InsertStatement parseInsert() {
    expectKeyword("into");
    InsertStatement statement;
    statement.table = expectIdentifier();
    const TableDef &table = requireTable(statement.table);
    if (consumeSymbol("(")) {
      while (true) {
        std::string column = expectIdentifier();
        table.columnIndex(column);
        statement.columns.push_back(std::move(column));
        if (!consumeSymbol(",")) {
          break;
        }
      }
      expectSymbol(")");
    }
    expectKeyword("values");
    while (true) {
      expectSymbol("(");
      Row expanded(table.columns.size());
      std::vector<bool> present(table.columns.size(), false);
      size_t value_index = 0;
      while (true) {
        size_t column_index = 0;
        if (statement.columns.empty()) {
          if (value_index >= table.columns.size()) {
            fail("TYPE_MISMATCH", "too many values for INSERT");
          }
          column_index = value_index;
        } else {
          if (value_index >= statement.columns.size()) {
            fail("TYPE_MISMATCH", "too many values for INSERT");
          }
          column_index = table.columnIndex(statement.columns[value_index]);
        }
        const Column &column = table.columns[column_index];
        if (present[column_index]) {
          fail("TYPE_MISMATCH", "duplicate column in INSERT");
        }
        expanded[column_index] = parseTypedValue(column.type, column.length);
        present[column_index] = true;
        ++value_index;
        if (!consumeSymbol(",")) {
          break;
        }
      }
      expectSymbol(")");
      for (size_t i = 0; i < present.size(); ++i) {
        if (!present[i]) {
          fail("TYPE_MISMATCH", "INSERT must provide every column");
        }
      }
      statement.rows.push_back(std::move(expanded));
      if (!consumeSymbol(",")) {
        break;
      }
    }
    expectEnd();
    return statement;
  }

  UpdateStatement parseUpdate() {
    UpdateStatement statement;
    statement.table = expectIdentifier();
    const TableDef &table = requireTable(statement.table);
    expectKeyword("set");
    while (true) {
      Assignment assignment;
      assignment.column = table.columnIndex(expectIdentifier());
      const Column &column = table.columns[assignment.column];
      expectSymbol("=");
      assignment.value = parseTypedValue(column.type, column.length);
      statement.assignments.push_back(std::move(assignment));
      if (!consumeSymbol(",")) {
        break;
      }
    }
    statement.conditions = parseConditions(table, true);
    expectEnd();
    return statement;
  }

  DeleteStatement parseDelete() {
    DeleteStatement statement;
    expectKeyword("from");
    statement.table = expectIdentifier();
    const TableDef &table = requireTable(statement.table);
    statement.conditions = parseConditions(table, true);
    expectEnd();
    return statement;
  }

  CreateTableStatement parseCreateTable() {
    TableDef table;
    table.name = expectIdentifier();
    expectSymbol("(");
    bool saw_primary = false;
    while (true) {
      if (tokenIsKeyword(peek(), "primary")) {
        if (saw_primary) {
          fail("PRIMARY_KEY_REQUIRED", "a table supports exactly one primary key");
        }
        ++pos_;
        expectKeyword("key");
        expectSymbol("(");
        std::string pk_name = expectIdentifier();
        expectSymbol(")");
        bool found_primary = false;
        for (size_t i = 0; i < table.columns.size(); ++i) {
          if (lower(table.columns[i].name) == lower(pk_name)) {
            table.primary_key = i;
            found_primary = true;
            saw_primary = true;
            break;
          }
        }
        if (!found_primary) {
          fail("UNKNOWN_COLUMN", "primary key column does not exist");
        }
      } else {
        Column column;
        column.name = expectIdentifier();
        for (const Column &existing : table.columns) {
          if (lower(existing.name) == lower(column.name)) {
            fail("SQL_SYNTAX", "duplicate column name '" + column.name + "'");
          }
        }
        std::string type = lower(expectIdentifier());
        if (type == "bigint") {
          column.type = ColumnType::BigInt;
        } else if (type == "varchar" || type == "char" || type == "text") {
          column.type = ColumnType::VarChar;
          if (consumeSymbol("(")) {
            int64_t length = parseNonNegativeInteger();
            if (length <= 0 || length > 65535) {
              fail("VALUE_OUT_OF_RANGE", "invalid VARCHAR length");
            }
            column.length = static_cast<uint32_t>(length);
            expectSymbol(")");
          } else {
            column.length = 65535;
          }
        } else if (type == "vector") {
          column.type = ColumnType::Vector;
          expectSymbol("(");
          int64_t dimension = parseNonNegativeInteger();
          if (dimension <= 0 || dimension > 65535) {
            fail("VECTOR_DIMENSION_MISMATCH", "invalid VECTOR dimension");
          }
          column.length = static_cast<uint32_t>(dimension);
          expectSymbol(")");
        } else {
          fail("UNSUPPORTED", "unsupported column type '" + type + "'");
        }
        while (true) {
          if (acceptKeyword("primary")) {
            if (saw_primary) {
              fail("PRIMARY_KEY_REQUIRED", "a table supports exactly one primary key");
            }
            expectKeyword("key");
            table.primary_key = table.columns.size();
            saw_primary = true;
          } else if (acceptKeyword("not")) {
            expectKeyword("null");
          } else {
            break;
          }
        }
        table.columns.push_back(std::move(column));
      }
      if (!consumeSymbol(",")) {
        break;
      }
    }
    expectSymbol(")");
    expectEnd();
    if (!saw_primary) {
      fail("PRIMARY_KEY_REQUIRED", "table requires one BIGINT primary key");
    }
    if (table.columns[table.primary_key].type != ColumnType::BigInt) {
      fail("PRIMARY_KEY_REQUIRED", "primary key must be BIGINT");
    }
    size_t vector_count = 0;
    for (size_t i = 0; i < table.columns.size(); ++i) {
      if (table.columns[i].type == ColumnType::Vector) {
        ++vector_count;
        table.vector_column = i;
      }
    }
    if (vector_count > 1) {
      fail("UNSUPPORTED", "a table supports at most one VECTOR column");
    }
    CreateTableStatement result;
    result.definition = std::move(table);
    return result;
  }

  CreateIndexStatement parseCreateIndex() {
    (void)expectIdentifier();
    expectKeyword("on");
    CreateIndexStatement statement;
    statement.table = expectIdentifier();
    const TableDef &table = requireTable(statement.table);
    expectSymbol("(");
    statement.column = table.columnIndex(expectIdentifier());
    expectSymbol(")");
    expectEnd();
    if (table.columns[statement.column].type != ColumnType::Vector) {
      fail("TYPE_MISMATCH", "vector index must target a VECTOR column");
    }
    return statement;
  }

  SetModeStatement parseSetMode() {
    expectKeyword("vector_mode");
    expectSymbol("=");
    std::string mode = lower(expectIdentifier());
    if (mode != "exact" && mode != "ann") {
      fail("UNSUPPORTED", "vector_mode must be exact or ann");
    }
    expectEnd();
    return SetModeStatement{mode == "ann"};
  }

  std::vector<Token> tokens_;
  size_t pos_ = 0;
  const std::unordered_map<std::string, TableDef> &tables_;
};

struct DecodedRecord {
  uint16_t op = 0;
  uint64_t txid = 0;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> encodeRowRecord(uint64_t txid, uint16_t op,
                                     const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> out;
  out.reserve(32 + payload.size());
  appendScalar<uint32_t>(out, kRowMagic);
  appendScalar<uint16_t>(out, kFormatVersion);
  appendScalar<uint16_t>(out, op);
  appendScalar<uint64_t>(out, txid);
  appendScalar<uint64_t>(out, static_cast<uint64_t>(payload.size()));
  appendScalar<uint32_t>(out, crc32(payload));
  appendScalar<uint32_t>(out, 0);
  appendBytes(out, payload.data(), payload.size());
  return out;
}

bool decodeRowHeader(const std::array<uint8_t, 32> &bytes, DecodedRecord &record,
                     uint64_t &payload_size) {
  std::vector<uint8_t> header(bytes.begin(), bytes.end());
  size_t pos = 0;
  if (readScalar<uint32_t>(header, pos) != kRowMagic ||
      readScalar<uint16_t>(header, pos) != kFormatVersion) {
    return false;
  }
  record.op = readScalar<uint16_t>(header, pos);
  record.txid = readScalar<uint64_t>(header, pos);
  payload_size = readScalar<uint64_t>(header, pos);
  return true;
}

uint32_t rowPayloadChecksum(const std::array<uint8_t, 32> &bytes) {
  std::vector<uint8_t> header(bytes.begin(), bytes.end());
  size_t pos = 24;
  return readScalar<uint32_t>(header, pos);
}

std::vector<uint8_t> encodeCommitRecord(uint64_t txid, uint64_t entries) {
  std::vector<uint8_t> payload;
  appendScalar<uint64_t>(payload, entries);
  std::vector<uint8_t> out;
  appendScalar<uint32_t>(out, kCommitMagic);
  appendScalar<uint16_t>(out, kFormatVersion);
  appendScalar<uint16_t>(out, 0);
  appendScalar<uint64_t>(out, txid);
  appendScalar<uint64_t>(out, static_cast<uint64_t>(payload.size()));
  appendScalar<uint32_t>(out, crc32(payload));
  appendScalar<uint32_t>(out, 0);
  appendBytes(out, payload.data(), payload.size());
  return out;
}

std::vector<uint8_t> serializeTableDefinition(const TableDef &table) {
  std::vector<uint8_t> out;
  appendScalar<uint32_t>(out, table.oid);
  appendString(out, table.name);
  appendScalar<uint32_t>(out, static_cast<uint32_t>(table.columns.size()));
  for (const Column &column : table.columns) {
    appendScalar<uint8_t>(out, static_cast<uint8_t>(column.type));
    appendString(out, column.name);
    appendScalar<uint32_t>(out, column.length);
  }
  appendScalar<uint32_t>(out, static_cast<uint32_t>(table.primary_key));
  appendScalar<uint32_t>(
      out, table.vector_column == std::numeric_limits<size_t>::max()
               ? std::numeric_limits<uint32_t>::max()
               : static_cast<uint32_t>(table.vector_column));
  appendScalar<uint8_t>(out, table.vector_index ? 1 : 0);
  return out;
}

TableDef deserializeTableDefinition(const std::vector<uint8_t> &data) {
  size_t pos = 0;
  TableDef table;
  table.oid = readScalar<uint32_t>(data, pos);
  table.name = readString(data, pos);
  uint32_t count = readScalar<uint32_t>(data, pos);
  if (count == 0 || count > 65535) {
    fail("CORRUPT_DATA", "invalid catalog column count");
  }
  table.columns.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Column column;
    uint8_t type = readScalar<uint8_t>(data, pos);
    if (type < 1 || type > 3) {
      fail("CORRUPT_DATA", "invalid catalog column type");
    }
    column.type = static_cast<ColumnType>(type);
    column.name = readString(data, pos);
    column.length = readScalar<uint32_t>(data, pos);
    table.columns.push_back(std::move(column));
  }
  table.primary_key = readScalar<uint32_t>(data, pos);
  uint32_t vector_column = readScalar<uint32_t>(data, pos);
  table.vector_column =
      vector_column == std::numeric_limits<uint32_t>::max()
          ? std::numeric_limits<size_t>::max()
          : vector_column;
  table.vector_index = readScalar<uint8_t>(data, pos) != 0;
  if (pos != data.size() || table.primary_key >= table.columns.size()) {
    fail("CORRUPT_DATA", "invalid catalog table definition");
  }
  return table;
}

std::vector<uint8_t> encodeCatalogRecord(const TableDef &table) {
  std::vector<uint8_t> payload = serializeTableDefinition(table);
  std::vector<uint8_t> out;
  appendScalar<uint32_t>(out, kCatalogMagic);
  appendScalar<uint16_t>(out, kFormatVersion);
  appendScalar<uint16_t>(out, 0);
  appendScalar<uint64_t>(out, static_cast<uint64_t>(payload.size()));
  appendScalar<uint32_t>(out, crc32(payload));
  appendScalar<uint32_t>(out, 0);
  appendBytes(out, payload.data(), payload.size());
  return out;
}

bool readExact(std::ifstream &input, void *destination, size_t size) {
  input.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
  return static_cast<size_t>(input.gcount()) == size;
}

struct ScanStats {
  std::unordered_set<uint64_t> committed;
  uint64_t max_txid = 0;
  uint64_t valid_bytes = 0;
};

ScanStats scanCommitLog(const fs::path &path) {
  ScanStats stats;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return stats;
  }
  while (true) {
    std::array<uint8_t, 32> header{};
    if (!readExact(input, header.data(), header.size())) {
      break;
    }
    std::vector<uint8_t> bytes(header.begin(), header.end());
    size_t pos = 0;
    if (readScalar<uint32_t>(bytes, pos) != kCommitMagic ||
        readScalar<uint16_t>(bytes, pos) != kFormatVersion) {
      break;
    }
    pos = 8;
    uint64_t txid = readScalar<uint64_t>(bytes, pos);
    uint64_t payload_size = readScalar<uint64_t>(bytes, pos);
    uint32_t checksum = readScalar<uint32_t>(bytes, pos);
    if (payload_size > (1ULL << 20ULL)) {
      break;
    }
    std::vector<uint8_t> payload(payload_size);
    if (!payload.empty() &&
        !readExact(input, payload.data(), static_cast<size_t>(payload_size))) {
      break;
    }
    if (crc32(payload) != checksum) {
      break;
    }
    stats.committed.insert(txid);
    stats.max_txid = std::max(stats.max_txid, txid);
    stats.valid_bytes += header.size() + payload_size;
  }
  return stats;
}

class FileWriter {
 public:
  explicit FileWriter(const fs::path &path) : path_(path) {
    file_ = std::fopen(path.string().c_str(), "ab");
    if (file_ == nullptr) {
      fail("IO_ERROR", "cannot open " + path.string() + " for append: " +
                           std::strerror(errno));
    }
#ifdef _WIN32
    if (_fseeki64(file_, 0, SEEK_END) != 0) {
      fail("IO_ERROR", "cannot seek " + path.string());
    }
    long long current = _ftelli64(file_);
#else
    if (std::fseek(file_, 0, SEEK_END) != 0) {
      fail("IO_ERROR", "cannot seek " + path.string());
    }
    off_t current = ftello(file_);
#endif
    if (current >= 0) {
      offset_ = static_cast<uint64_t>(current);
    }
  }

  FileWriter(const FileWriter &) = delete;
  FileWriter &operator=(const FileWriter &) = delete;

  ~FileWriter() {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
  }

  uint64_t append(const std::vector<uint8_t> &data) {
    const uint64_t start = offset_;
    if (!data.empty() &&
        std::fwrite(data.data(), 1, data.size(), file_) != data.size()) {
      fail("IO_ERROR", "short write to " + path_.string());
    }
    offset_ += data.size();
    return start;
  }

  void sync() {
    if (std::fflush(file_) != 0) {
      fail("IO_ERROR", "flush failed for " + path_.string());
    }
    int descriptor = -1;
#ifdef _WIN32
    descriptor = _fileno(file_);
    if (descriptor < 0 || _commit(descriptor) != 0) {
      fail("IO_ERROR", "fsync failed for " + path_.string());
    }
#else
    descriptor = fileno(file_);
    if (descriptor < 0 || fsync(descriptor) != 0) {
      fail("IO_ERROR", "fsync failed for " + path_.string());
    }
#endif
  }

  uint64_t offset() const { return offset_; }

 private:
  fs::path path_;
  FILE *file_ = nullptr;
  uint64_t offset_ = 0;
};

struct Version {
  uint64_t offset = 0;
  bool cached = false;
  Row row;
};

struct PendingRecord {
  uint32_t oid = 0;
  int64_t primary = 0;
  uint16_t op = 0;
  Row row;
  uint64_t offset = 0;
};

class Table {
 public:
  Table(TableDef definition, fs::path data_path)
      : definition_(std::move(definition)), data_path_(std::move(data_path)) {}

  void load(const std::unordered_set<uint64_t> &committed,
            uint64_t &max_seen_txid) {
    std::ifstream input(data_path_, std::ios::binary);
    uint64_t offset = 0;
    uint64_t valid_bytes = 0;
    if (input) {
      while (true) {
        const uint64_t record_offset = offset;
        std::array<uint8_t, 32> header{};
        if (!readExact(input, header.data(), header.size())) {
          break;
        }
        DecodedRecord record;
        uint64_t payload_size = 0;
        if (!decodeRowHeader(header, record, payload_size) ||
            payload_size > (1ULL << 31ULL)) {
          break;
        }
        std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
        if (!payload.empty() &&
            !readExact(input, payload.data(), payload.size())) {
          break;
        }
        if (crc32(payload) != rowPayloadChecksum(header)) {
          break;
        }
        offset += header.size() + payload_size;
        valid_bytes = offset;
        // Never reuse an ID left by an interrupted, uncommitted transaction.
        max_seen_txid = std::max(max_seen_txid, record.txid + 1);
        if (committed.contains(record.txid)) {
          applyRecord(record, payload, record_offset);
        }
      }
    }
    next_offset_ = valid_bytes;
    if (fs::exists(data_path_) && fs::file_size(data_path_) != valid_bytes) {
      fs::resize_file(data_path_, valid_bytes);
    }
  }

  const TableDef &definition() const { return definition_; }
  TableDef &definition() { return definition_; }
  const fs::path &dataPath() const { return data_path_; }

  void setCacheLimit(size_t bytes) {
    clearEqualityIndex();
    cache_limit_bytes_ = bytes;
    if (cache_bytes_ > cache_limit_bytes_) {
      disableCache();
    }
  }

  bool findBase(int64_t primary, Row &out) const {
    auto found = versions_.find(primary);
    if (found == versions_.end()) {
      return false;
    }
    if (found->second.cached) {
      out = found->second.row;
    } else {
      out = readRow(found->second.offset);
    }
    return true;
  }

  bool containsBase(int64_t primary) const {
    return versions_.contains(primary);
  }

  using IndexedRows = std::vector<std::pair<int64_t, const Row *>>;

  const IndexedRows *equalityCandidates(const Condition &condition) {
    if (!cache_enabled_) {
      return nullptr;
    }
    const size_t column = condition.column;
    const bool text = definition_.columns[column].type == ColumnType::VarChar;
    const auto keyFor = [&](const Value &value) {
      return text ? value.text : std::to_string(value.integer);
    };
    if (equality_column_ != column) {
      clearEqualityIndex();
      // Build off to the side: an allocation failure must never publish a
      // partially populated candidate index to a later successful query.
      decltype(equality_index_) staged;
      const size_t available = cache_limit_bytes_ > cache_bytes_
                                   ? cache_limit_bytes_ - cache_bytes_
                                   : 0;
      size_t allocated = 0;
      for (const auto &[primary, version] : versions_) {
        std::string key = keyFor(version.row[column]);
        auto [entry, inserted] = staged.try_emplace(std::move(key));
        if (inserted) {
          allocated += sizeof(decltype(equality_index_)::value_type) + 64 +
                       entry->first.capacity() + 1;
        }
        IndexedRows &rows = entry->second;
        const size_t old_capacity = rows.capacity();
        rows.emplace_back(primary, &version.row);
        allocated += (rows.capacity() - old_capacity) * sizeof(IndexedRows::value_type);
        if (allocated + staged.bucket_count() * sizeof(void *) > available) {
          equality_column_ = column;
          equality_disabled_ = true;
          return nullptr;
        }
      }
      equality_index_.swap(staged);
      equality_column_ = column;
    }
    if (equality_disabled_) {
      return nullptr;
    }
    const auto found = equality_index_.find(keyFor(condition.value));
    static const IndexedRows empty;
    return found == equality_index_.end() ? &empty : &found->second;
  }

  template <typename Callback>
  void forEachBase(Callback &&callback) const {
    // Reuse one stream for an entire out-of-cache scan, rather than opening
    // and closing the same data file for every row.
    std::ifstream input;
    for (const auto &[primary, version] : versions_) {
      if (version.cached) {
        callback(primary, version.row);
      } else {
        if (!input.is_open()) {
          input.open(data_path_, std::ios::binary);
        }
        Row row = readRow(version.offset, input);
        callback(primary, row);
      }
    }
  }

  std::vector<PendingRecord> appendChanges(uint64_t txid,
                                           FileWriter &writer,
                                           const std::map<int64_t, std::optional<Row>>
                                               &changes) {
    std::vector<PendingRecord> pending;
    pending.reserve(changes.size());
    for (const auto &[primary, row] : changes) {
      std::vector<uint8_t> payload;
      uint16_t op = 0;
      if (row.has_value()) {
        op = containsBase(primary) ? static_cast<uint16_t>(2)
                                   : static_cast<uint16_t>(1);
        payload = serializeRow(*row);
      } else {
        op = 3;
        appendScalar<int64_t>(payload, primary);
      }
      std::vector<uint8_t> record = encodeRowRecord(txid, op, payload);
      const uint64_t offset = writer.append(record);
      pending.push_back({definition_.oid, primary, op,
                         row.has_value() ? *row : Row{}, offset});
      next_offset_ = writer.offset();
    }
    return pending;
  }

  void applyPending(const std::vector<PendingRecord> &pending) {
    // Entries point to immutable committed row objects. Invalidate before
    // replacing/deleting a row or changing the cache's residency.
    clearEqualityIndex();
    for (const PendingRecord &record : pending) {
      if (record.oid != definition_.oid) {
        continue;
      }
      if (record.op == 3) {
        auto existing = versions_.find(record.primary);
        if (existing != versions_.end() && existing->second.cached) {
          const size_t bytes = cachedRowBytes(existing->second.row);
          cache_bytes_ = cache_bytes_ > bytes ? cache_bytes_ - bytes : 0;
        }
        versions_.erase(record.primary);
        continue;
      }
      auto existing = versions_.find(record.primary);
      if (existing != versions_.end() && existing->second.cached) {
        const size_t bytes = cachedRowBytes(existing->second.row);
        cache_bytes_ = cache_bytes_ > bytes ? cache_bytes_ - bytes : 0;
      }
      Version value;
      value.offset = record.offset;
      value.cached = cache_enabled_;
      if (value.cached) {
        value.row = record.row;
        cache_bytes_ += cachedRowBytes(value.row);
      }
      versions_[record.primary] = std::move(value);
      if (cache_bytes_ > cache_limit_bytes_) {
        disableCache();
      }
    }
  }

  size_t rowCount() const { return versions_.size(); }

 private:
  void applyRecord(const DecodedRecord &record,
                   const std::vector<uint8_t> &payload, uint64_t offset) {
    if (record.op == 3) {
      if (payload.size() != sizeof(int64_t)) {
        fail("CORRUPT_DATA", "invalid delete record");
      }
      size_t pos = 0;
      int64_t primary = readScalar<int64_t>(payload, pos);
      auto existing = versions_.find(primary);
      if (existing != versions_.end() && existing->second.cached) {
        const size_t bytes = cachedRowBytes(existing->second.row);
        cache_bytes_ = cache_bytes_ > bytes ? cache_bytes_ - bytes : 0;
      }
      versions_.erase(primary);
      return;
    }
    Row row = deserializeRow(payload);
    int64_t primary = row.at(definition_.primary_key).integer;
    Version version;
    version.offset = offset;
    version.cached = cache_enabled_;
    if (version.cached) {
      version.row = row;
      auto existing = versions_.find(primary);
      if (existing != versions_.end() && existing->second.cached) {
        const size_t bytes = cachedRowBytes(existing->second.row);
        cache_bytes_ = cache_bytes_ > bytes ? cache_bytes_ - bytes : 0;
      }
      cache_bytes_ += cachedRowBytes(version.row);
    }
    versions_[primary] = std::move(version);
    if (cache_bytes_ > cache_limit_bytes_) {
      disableCache();
    }
  }

  Row readRow(uint64_t offset) const {
    std::ifstream input(data_path_, std::ios::binary);
    return readRow(offset, input);
  }

  Row readRow(uint64_t offset, std::ifstream &input) const {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<uint8_t, 32> header{};
    if (!readExact(input, header.data(), header.size())) {
      fail("CORRUPT_DATA", "row record is missing");
    }
    DecodedRecord record;
    uint64_t payload_size = 0;
    if (!decodeRowHeader(header, record, payload_size) ||
        payload_size > (1ULL << 31ULL)) {
      fail("CORRUPT_DATA", "invalid row record");
    }
    std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
    if (!payload.empty() && !readExact(input, payload.data(), payload.size())) {
      fail("CORRUPT_DATA", "row payload is truncated");
    }
    if (crc32(payload) != rowPayloadChecksum(header) || record.op == 3) {
      fail("CORRUPT_DATA", "invalid row payload");
    }
    return deserializeRow(payload);
  }

  void disableCache() {
    clearEqualityIndex();
    cache_enabled_ = false;
    cache_bytes_ = 0;
    for (auto &[primary, version] : versions_) {
      (void)primary;
      version.cached = false;
      Row{}.swap(version.row);
    }
  }

  TableDef definition_;
  fs::path data_path_;
  std::unordered_map<int64_t, Version> versions_;
  bool cache_enabled_ = true;
  size_t cache_bytes_ = 0;
  size_t cache_limit_bytes_ = cacheLimitBytes();
  uint64_t next_offset_ = 0;
  size_t equality_column_ = std::numeric_limits<size_t>::max();
  bool equality_disabled_ = false;
  std::unordered_map<std::string, IndexedRows> equality_index_;

  void clearEqualityIndex() {
    decltype(equality_index_){}.swap(equality_index_);
    equality_column_ = std::numeric_limits<size_t>::max();
    equality_disabled_ = false;
  }
};

struct ExecResult {
  bool select = false;
  std::optional<uint64_t> affected;
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> columns{};
};

bool valuesCompare(const Value &left, const Value &right, CompareOp op,
                   ColumnType type) {
  int comparison = 0;
  if (type == ColumnType::BigInt) {
    comparison = left.integer < right.integer
                     ? -1
                     : (left.integer > right.integer ? 1 : 0);
  } else if (type == ColumnType::VarChar) {
    comparison = left.text < right.text ? -1 : (left.text > right.text ? 1 : 0);
  } else {
    fail("UNSUPPORTED", "VECTOR values are not comparable with WHERE operators");
  }
  switch (op) {
    case CompareOp::Eq:
      return comparison == 0;
    case CompareOp::Lt:
      return comparison < 0;
    case CompareOp::Le:
      return comparison <= 0;
    case CompareOp::Gt:
      return comparison > 0;
    case CompareOp::Ge:
      return comparison >= 0;
  }
  return false;
}

class Database {
 public:
  explicit Database(fs::path data_dir, bool ann)
      : data_dir_(std::move(data_dir)), ann_mode_(ann) {
    fs::create_directories(data_dir_);
    loadCatalog();
    rebalanceRowCaches();
    ScanStats wal = scanCommitLog(data_dir_ / "mvd.wal");
    committed_txids_ = wal.committed;
    next_txid_ = wal.max_txid + 1;
    for (auto &[name, table] : tables_) {
      (void)name;
      table->load(committed_txids_, next_txid_);
    }
    if (fs::exists(data_dir_ / "mvd.wal") &&
        fs::file_size(data_dir_ / "mvd.wal") != wal.valid_bytes) {
      fs::resize_file(data_dir_ / "mvd.wal", wal.valid_bytes);
    }
    wal_writer_ = std::make_unique<FileWriter>(data_dir_ / "mvd.wal");
  }

  ~Database() { rollbackSilently(); }

  ExecResult execute(std::string_view sql) {
    auto definitions = tableDefinitions();
    Parser parser(sql, definitions);
    const std::string first = parser.firstKeyword();
    Statement statement = parser.parse();
    if (std::holds_alternative<EmptyStatement>(statement)) {
      if (first == "begin" || first == "start") {
        if (explicit_transaction_) {
          fail("TRANSACTION_ACTIVE", "a transaction is already active");
        }
        explicit_transaction_ = true;
        overlay_.clear();
      } else if (first == "commit" || first == "end") {
        commitTransaction();
        explicit_transaction_ = false;
      } else if (first == "rollback") {
        rollbackSilently();
        explicit_transaction_ = false;
      }
      return ExecResult{};
    }
    if (std::holds_alternative<SetModeStatement>(statement)) {
      ann_mode_ = std::get<SetModeStatement>(statement).ann;
      return ExecResult{};
    }
    if (std::holds_alternative<CreateTableStatement>(statement)) {
      return createTable(std::get<CreateTableStatement>(statement).definition);
    }
    if (std::holds_alternative<CreateIndexStatement>(statement)) {
      return createIndex(std::get<CreateIndexStatement>(statement));
    }
    if (std::holds_alternative<InsertStatement>(statement)) {
      ExecResult result = executeInsert(std::get<InsertStatement>(statement));
      finishMutation();
      return result;
    }
    if (std::holds_alternative<UpdateStatement>(statement)) {
      ExecResult result = executeUpdate(std::get<UpdateStatement>(statement));
      finishMutation();
      return result;
    }
    if (std::holds_alternative<DeleteStatement>(statement)) {
      ExecResult result = executeDelete(std::get<DeleteStatement>(statement));
      finishMutation();
      return result;
    }
    const SelectStatement &select = std::get<SelectStatement>(statement);
    ExecResult result = executeSelect(select);
    const TableDef &definition = requireTable(select.table).definition();
    for (const SelectItem &item : select.items) {
      if (item.star) {
        for (const Column &column : definition.columns) {
          result.columns.push_back(column.name);
        }
      } else if (!item.hidden) {
        result.columns.push_back(item.output_name);
      }
    }
    return result;
  }

  void rollbackSilently() noexcept {
    overlay_.clear();
    explicit_transaction_ = false;
  }

 private:
  struct Projection {
    bool distance = false;
    size_t column = 0;
    std::string name;
  };

  struct SelectedRow {
    Row source;
    std::vector<std::string> output;
    std::vector<double> distances;
  };

  struct ResolvedOrder {
    bool distance = false;
    size_t column = 0;
    size_t distance_index = 0;
    ColumnType column_type = ColumnType::BigInt;
    bool descending = false;
  };

  std::unordered_map<std::string, TableDef> tableDefinitions() const {
    std::unordered_map<std::string, TableDef> result;
    for (const auto &[name, table] : tables_) {
      result.emplace(name, table->definition());
    }
    return result;
  }

  Table &requireTable(std::string_view name) {
    auto found = tables_.find(lower(std::string(name)));
    if (found == tables_.end()) {
      fail("UNKNOWN_TABLE", "unknown table '" + std::string(name) + "'");
    }
    return *found->second;
  }

  void loadCatalog() {
    const fs::path path = data_dir_ / "catalog.bin";
    std::ifstream input(path, std::ios::binary);
    uint64_t offset = 0;
    uint64_t valid_bytes = 0;
    if (input) {
      while (true) {
        std::array<uint8_t, 24> header{};
        if (!readExact(input, header.data(), header.size())) {
          break;
        }
        std::vector<uint8_t> bytes(header.begin(), header.end());
        size_t pos = 0;
        if (readScalar<uint32_t>(bytes, pos) != kCatalogMagic ||
            readScalar<uint16_t>(bytes, pos) != kFormatVersion) {
          break;
        }
        pos = 8;
        uint64_t payload_size = readScalar<uint64_t>(bytes, pos);
        uint32_t checksum = readScalar<uint32_t>(bytes, pos);
        if (payload_size > (1ULL << 24ULL)) {
          break;
        }
        std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
        if (!payload.empty() &&
            !readExact(input, payload.data(), payload.size())) {
          break;
        }
        if (crc32(payload) != checksum) {
          break;
        }
        TableDef definition = deserializeTableDefinition(payload);
        const std::string key = lower(definition.name);
        tables_[key] = std::make_unique<Table>(
            definition, data_dir_ / ("t" + std::to_string(definition.oid) + ".dat"));
        next_oid_ = std::max(next_oid_, definition.oid + 1);
        offset += header.size() + payload_size;
        valid_bytes = offset;
      }
    }
    if (fs::exists(path) && fs::file_size(path) != valid_bytes) {
      fs::resize_file(path, valid_bytes);
    }
  }

  void appendCatalog(const TableDef &definition) {
    FileWriter writer(data_dir_ / "catalog.bin");
    writer.append(encodeCatalogRecord(definition));
    writer.sync();
  }

  ExecResult createTable(TableDef definition) {
    const std::string key = lower(definition.name);
    if (tables_.contains(key)) {
      fail("TABLE_EXISTS", "table '" + definition.name + "' already exists");
    }
    definition.oid = next_oid_++;
    appendCatalog(definition);
    tables_[key] = std::make_unique<Table>(
        definition, data_dir_ / ("t" + std::to_string(definition.oid) + ".dat"));
    rebalanceRowCaches();
    return ExecResult{};
  }

  void rebalanceRowCaches() {
    const size_t per_table = cacheLimitBytes() / std::max<size_t>(1, tables_.size());
    for (auto &[name, table] : tables_) {
      (void)name;
      table->setCacheLimit(per_table);
    }
  }

  ExecResult createIndex(const CreateIndexStatement &statement) {
    Table &table = requireTable(statement.table);
    TableDef updated = table.definition();
    updated.vector_index = true;
    appendCatalog(updated);
    table.definition() = std::move(updated);
    return ExecResult{};
  }

  void finishMutation() {
    if (!explicit_transaction_) {
      try {
        commitTransaction();
      } catch (...) {
        overlay_.clear();
        throw;
      }
      overlay_.clear();
    }
  }

  void commitTransaction() {
    if (overlay_.empty()) {
      return;
    }
    const uint64_t txid = next_txid_++;
    std::vector<PendingRecord> pending;
    std::unordered_map<uint32_t, std::unique_ptr<FileWriter>> writers;
    try {
      for (const auto &[oid, changes] : overlay_) {
        if (changes.empty()) {
          continue;
        }
        Table *table = tableByOid(oid);
        if (table == nullptr) {
          fail("CORRUPT_DATA", "transaction references an unknown table");
        }
        auto writer = std::make_unique<FileWriter>(table->dataPath());
        std::vector<PendingRecord> records =
            table->appendChanges(txid, *writer, changes);
        pending.insert(pending.end(), records.begin(), records.end());
        writer->sync();
        writers.emplace(oid, std::move(writer));
      }
      wal_writer_->append(encodeCommitRecord(txid, pending.size()));
      wal_writer_->sync();
      committed_txids_.insert(txid);
      for (auto &[name, table] : tables_) {
        (void)name;
        table->applyPending(pending);
      }
      overlay_.clear();
    } catch (...) {
      overlay_.clear();
      throw;
    }
  }

  Table *tableByOid(uint32_t oid) {
    for (auto &[name, table] : tables_) {
      (void)name;
      if (table->definition().oid == oid) {
        return table.get();
      }
    }
    return nullptr;
  }

  std::map<int64_t, std::optional<Row>> &overlayFor(uint32_t oid) {
    return overlay_[oid];
  }

  bool matches(const TableDef &definition, const Row &row,
               const std::vector<Condition> &conditions) const {
    for (const Condition &condition : conditions) {
      if (!valuesCompare(row[condition.column], condition.value, condition.op,
                         definition.columns[condition.column].type)) {
        return false;
      }
    }
    return true;
  }

  bool currentRow(Table &table, int64_t primary, Row &row) {
    auto overlay_table =
        overlay_.find(table.definition().oid);
    if (overlay_table != overlay_.end()) {
      auto changed = overlay_table->second.find(primary);
      if (changed != overlay_table->second.end()) {
        if (!changed->second.has_value()) {
          return false;
        }
        row = *changed->second;
        return true;
      }
    }
    return table.findBase(primary, row);
  }

  template <typename Callback>
  void forEachVisible(Table &table, Callback &&callback) {
    const uint32_t oid = table.definition().oid;
    const auto overlay_found = overlay_.find(oid);
    const std::map<int64_t, std::optional<Row>> *overlay =
        overlay_found == overlay_.end() ? nullptr : &overlay_found->second;
    table.forEachBase([&](int64_t primary, const Row &row) {
      if (overlay != nullptr) {
        auto changed = overlay->find(primary);
        if (changed != overlay->end()) {
          if (changed->second.has_value()) {
            callback(primary, *changed->second);
          }
          return;
        }
      }
      callback(primary, row);
    });
    if (overlay != nullptr) {
      for (const auto &[primary, row] : *overlay) {
        if (!table.containsBase(primary) && row.has_value()) {
          callback(primary, *row);
        }
      }
    }
  }

  ExecResult executeInsert(const InsertStatement &statement) {
    Table &table = requireTable(statement.table);
    const TableDef &definition = table.definition();
    if (statement.rows.empty()) {
      return ExecResult{false, uint64_t{0}, {}};
    }
    std::unordered_set<int64_t> batch_keys;
    for (const Row &row : statement.rows) {
      int64_t primary = row[definition.primary_key].integer;
      if (!batch_keys.insert(primary).second) {
        fail("DUPLICATE_PRIMARY_KEY", "duplicate primary key in INSERT");
      }
      Row existing;
      if (currentRow(table, primary, existing)) {
        fail("DUPLICATE_PRIMARY_KEY", "primary key already exists");
      }
    }
    auto &overlay = overlayFor(definition.oid);
    for (const Row &row : statement.rows) {
      overlay[row[definition.primary_key].integer] = row;
    }
    return ExecResult{false, static_cast<uint64_t>(statement.rows.size()), {}};
  }

  template <typename Callback>
  void forEachCandidate(Table &table, const std::vector<Condition> &conditions,
                        Callback &&callback) {
    for (const Condition &condition : conditions) {
      if (condition.column == table.definition().primary_key &&
          condition.op == CompareOp::Eq) {
        Row row;
        if (currentRow(table, condition.value.integer, row)) {
          callback(condition.value.integer, row);
        }
        return;
      }
    }
    for (const Condition &condition : conditions) {
      if (condition.op != CompareOp::Eq) {
        continue;
      }
      const auto *indexed = table.equalityCandidates(condition);
      if (indexed == nullptr) {
        continue;
      }
      const auto changed_table = overlay_.find(table.definition().oid);
      for (const auto &[primary, row] : *indexed) {
        if (changed_table == overlay_.end() ||
            !changed_table->second.contains(primary)) {
          callback(primary, *row);
        }
      }
      if (changed_table != overlay_.end()) {
        for (const auto &[primary, row] : changed_table->second) {
          if (row.has_value() &&
              valuesCompare((*row)[condition.column], condition.value,
                            CompareOp::Eq,
                            table.definition().columns[condition.column].type)) {
            callback(primary, *row);
          }
        }
      }
      return;
    }
    forEachVisible(table, std::forward<Callback>(callback));
  }

  ExecResult executeUpdate(const UpdateStatement &statement) {
    Table &table = requireTable(statement.table);
    const TableDef &definition = table.definition();
    for (const Assignment &assignment : statement.assignments) {
      if (assignment.column == definition.primary_key) {
        fail("UNSUPPORTED", "primary key updates are not supported");
      }
    }
    std::vector<std::pair<int64_t, Row>> replacements;
    forEachCandidate(table, statement.conditions, [&](int64_t primary, const Row &row) {
      if (!matches(definition, row, statement.conditions)) {
        return;
      }
      Row updated = row;
      for (const Assignment &assignment : statement.assignments) {
        updated[assignment.column] = assignment.value;
      }
      replacements.emplace_back(primary, std::move(updated));
    });
    auto &overlay = overlayFor(definition.oid);
    for (auto &[primary, row] : replacements) {
      overlay[primary] = std::move(row);
    }
    return ExecResult{false, static_cast<uint64_t>(replacements.size()), {}};
  }

  ExecResult executeDelete(const DeleteStatement &statement) {
    Table &table = requireTable(statement.table);
    const TableDef &definition = table.definition();
    std::vector<int64_t> matches_to_delete;
    forEachCandidate(table, statement.conditions, [&](int64_t primary, const Row &row) {
      if (matches(definition, row, statement.conditions)) {
        matches_to_delete.push_back(primary);
      }
    });
    auto &overlay = overlayFor(definition.oid);
    for (int64_t primary : matches_to_delete) {
      overlay[primary] = std::nullopt;
    }
    return ExecResult{false,
                      static_cast<uint64_t>(matches_to_delete.size()), {}};
  }

  ExecResult executeSelect(const SelectStatement &statement) {
    Table &table = requireTable(statement.table);
    const TableDef &definition = table.definition();
    std::vector<Projection> projections;
    for (const SelectItem &item : statement.items) {
      if (item.hidden) {
        continue;
      }
      if (item.star) {
        for (size_t i = 0; i < definition.columns.size(); ++i) {
          projections.push_back({false, i, definition.columns[i].name});
        }
      } else {
        projections.push_back(
            {item.distance, item.column, item.output_name});
      }
    }
    std::vector<ResolvedOrder> resolved_order;
    resolved_order.reserve(statement.order.size());
    for (const OrderKey &key : statement.order) {
      ResolvedOrder resolved;
      resolved.descending = key.descending;
      bool found = false;
      if (key.distance) {
        const size_t source = definition.columnIndex(key.source_name);
        if (definition.columns[source].type != ColumnType::Vector ||
            definition.columns[source].length != key.probe.size()) {
          fail("VECTOR_DIMENSION_MISMATCH",
               "ORDER BY distance vector does not match schema");
        }
        for (size_t i = 0; i < statement.items.size(); ++i) {
          if (statement.items[i].distance &&
              statement.items[i].column == source &&
              statement.items[i].probe == key.probe) {
            resolved.distance = true;
            resolved.column = source;
            resolved.distance_index = i;
            found = true;
            break;
          }
        }
        if (!found) {
          fail("UNSUPPORTED",
               "ORDER BY L2_DISTANCE must match a selected distance expression");
        }
      } else {
        for (size_t i = 0; i < statement.items.size(); ++i) {
          if (lower(statement.items[i].output_name) == lower(key.reference) &&
              !statement.items[i].star) {
            resolved.distance = statement.items[i].distance;
            resolved.column = statement.items[i].column;
            resolved.distance_index = i;
            found = true;
            break;
          }
        }
      }
      if (!found) {
        resolved.column = definition.columnIndex(key.reference);
        resolved.distance = false;
      }
      if (!resolved.distance) {
        resolved.column_type = definition.columns[resolved.column].type;
        if (resolved.column_type == ColumnType::Vector) {
          fail("UNSUPPORTED", "VECTOR columns cannot be ordered directly");
        }
      }
      resolved_order.push_back(resolved);
    }
    if (statement.limit == 0) {
      return ExecResult{true, std::nullopt, {}};
    }
    auto makeOutput = [&](const Row &row) {
      SelectedRow output;
      output.source = row;
      output.output.resize(projections.size());
      output.distances.assign(statement.items.size(),
                              std::numeric_limits<double>::quiet_NaN());
      for (size_t item_index = 0; item_index < statement.items.size();
           ++item_index) {
        const SelectItem &item = statement.items[item_index];
        if (item.distance) {
          output.distances[item_index] =
              l2Distance(row[item.column].vector, item.probe);
        }
      }
      size_t output_index = 0;
      for (size_t item_index = 0; item_index < statement.items.size();
           ++item_index) {
        const SelectItem &item = statement.items[item_index];
        if (item.hidden) {
          continue;
        }
        if (item.star) {
          for (size_t column = 0; column < definition.columns.size(); ++column) {
            output.output[output_index++] = valueToString(
                row[column], definition.columns[column].type);
          }
        } else if (item.distance) {
          std::ostringstream distance;
          distance.imbue(std::locale::classic());
          distance << std::fixed << std::setprecision(6)
                   << output.distances[item_index];
          output.output[output_index++] = distance.str();
        } else {
          output.output[output_index++] =
              valueToString(row[item.column], definition.columns[item.column].type);
        }
      }
      return output;
    };

    int distance_item = -1;
    for (size_t i = 0; i < statement.items.size(); ++i) {
      if (!statement.items[i].distance) {
        continue;
      }
      if (distance_item >= 0) {
        distance_item = -2;
        break;
      }
      distance_item = static_cast<int>(i);
    }
    if (statement.limit >= 0 && distance_item >= 0 &&
        resolved_order.size() == 1 && resolved_order.front().distance &&
        !resolved_order.front().descending &&
        resolved_order.front().distance_index ==
            static_cast<size_t>(distance_item)) {
      struct Candidate {
        double squared_distance = 0;
        int64_t primary = 0;
        Row row;
      };
      struct WorseFirst {
        size_t column;
        const std::vector<float> *probe;
        bool operator()(const Candidate &left, const Candidate &right) const {
          const int comparison = compareDistances(
              left.squared_distance, left.row[column].vector,
              right.squared_distance, right.row[column].vector, *probe);
          if (comparison != 0) {
            return comparison < 0;
          }
          return left.primary < right.primary;
        }
      };
      const SelectItem &distance_spec = statement.items[distance_item];
      // The probe is constant for the scan. Widen it and resolve the CPU kernel
      // once, retaining float32 storage and double-precision distance semantics.
      const std::vector<double> prepared_probe(distance_spec.probe.begin(),
                                                distance_spec.probe.end());
      using DistanceKernel = double (*)(const float *, const double *, size_t);
      DistanceKernel distance_kernel = &squaredDistanceScalar<double>;
#if !defined(MVD_FORCE_SCALAR) && (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
      if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        distance_kernel = &squaredDistanceAvx2<double>;
      }
#endif
      const int64_t wanted =
          statement.offset >
                  std::numeric_limits<int64_t>::max() - statement.limit
              ? std::numeric_limits<int64_t>::max()
              : statement.offset + statement.limit;
      const size_t capacity = static_cast<size_t>(
          wanted);
      const WorseFirst candidateLess{distance_spec.column, &distance_spec.probe};
      std::priority_queue<Candidate, std::vector<Candidate>, WorseFirst> heap(candidateLess);
      // Most scanned rows are well outside the current TopK. Cache an outward
      // rounded rejection boundary after heap changes instead of rebuilding
      // the precision check (and calling its cold exact path) for every row.
      const double rejection_multiplier =
          1 + (static_cast<double>(prepared_probe.size()) + 16) * 16 *
                  std::numeric_limits<double>::epsilon();
      double reject_above = std::numeric_limits<double>::infinity();
      const auto updateRejectionBoundary = [&] {
        reject_above = std::nextafter(
            heap.top().squared_distance * rejection_multiplier,
            std::numeric_limits<double>::infinity());
      };
      forEachCandidate(table, statement.conditions, [&](int64_t primary, const Row &row) {
        if (!matches(definition, row, statement.conditions)) {
          return;
        }
        const auto &vector = row[distance_spec.column].vector;
        if (vector.size() != prepared_probe.size()) {
          fail("VECTOR_DIMENSION_MISMATCH", "vector dimension mismatch");
        }
        double squared = distance_kernel(vector.data(), prepared_probe.data(),
                                         prepared_probe.size());
        if (heap.size() < capacity) {
          heap.push(Candidate{squared, primary, row});
          updateRejectionBoundary();
        } else if (capacity == 0) {
          return;
        } else if (squared > reject_above) {
          return;
        } else {
          const Candidate &worst = heap.top();
          const int comparison = compareDistances(
              squared, vector, worst.squared_distance,
              worst.row[distance_spec.column].vector, distance_spec.probe);
          if (comparison < 0 || (comparison == 0 && primary < worst.primary)) {
            heap.pop();
            heap.push(Candidate{squared, primary, row});
            updateRejectionBoundary();
          }
        }
      });
      std::vector<Candidate> candidates;
      candidates.reserve(heap.size());
      while (!heap.empty()) {
        candidates.push_back(heap.top());
        heap.pop();
      }
      std::sort(candidates.begin(), candidates.end(), candidateLess);
      ExecResult result;
      result.select = true;
      if (statement.offset < static_cast<int64_t>(candidates.size())) {
        const size_t begin = static_cast<size_t>(statement.offset);
        const size_t end = std::min(
            candidates.size(),
            begin + static_cast<size_t>(statement.limit));
        result.rows.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
          result.rows.push_back(makeOutput(candidates[i].row).output);
        }
      }
      return result;
    }

    const auto selectedLess = [&](const SelectedRow &left, const SelectedRow &right) {
      for (const ResolvedOrder &key : resolved_order) {
        int comparison = 0;
        if (key.distance) {
          const double a = left.distances[key.distance_index];
          const double b = right.distances[key.distance_index];
          const SelectItem &item = statement.items[key.distance_index];
          comparison = compareDistances(a, left.source[item.column].vector,
                                        b, right.source[item.column].vector,
                                        item.probe);
        } else {
          comparison = compareValues(left.source[key.column], right.source[key.column],
                                     key.column_type);
        }
        if (comparison != 0) {
          return key.descending ? comparison > 0 : comparison < 0;
        }
      }
      return left.source[definition.primary_key].integer <
             right.source[definition.primary_key].integer;
    };
    const bool unlimited = statement.limit < 0 ||
        statement.offset > std::numeric_limits<int64_t>::max() - statement.limit;
    const int64_t wanted = unlimited ? std::numeric_limits<int64_t>::max()
                                    : statement.offset + statement.limit;
    const size_t capacity = static_cast<size_t>(wanted);
    std::vector<SelectedRow> selected;
    if (!resolved_order.empty() && statement.limit >= 0) {
      // Bounded selection applies the complete ordering, including secondary
      // keys, without retaining every vector row when only a small LIMIT is needed.
      std::priority_queue<SelectedRow, std::vector<SelectedRow>, decltype(selectedLess)>
          heap(selectedLess);
      forEachCandidate(table, statement.conditions, [&](int64_t, const Row &row) {
        if (!matches(definition, row, statement.conditions)) {
          return;
        }
        SelectedRow candidate = makeOutput(row);
        if (heap.size() < capacity) {
          heap.push(std::move(candidate));
        } else if (selectedLess(candidate, heap.top())) {
          heap.pop();
          heap.push(std::move(candidate));
        }
      });
      selected.reserve(heap.size());
      while (!heap.empty()) {
        selected.push_back(heap.top());
        heap.pop();
      }
    } else {
      forEachCandidate(table, statement.conditions, [&](int64_t, const Row &row) {
        if (selected.size() < capacity && matches(definition, row, statement.conditions)) {
          selected.push_back(makeOutput(row));
        }
      });
    }
    if (!resolved_order.empty()) {
      std::sort(selected.begin(), selected.end(), selectedLess);
    }

    int64_t begin = std::min<int64_t>(statement.offset, selected.size());
    int64_t end = selected.size();
    if (statement.limit >= 0) {
      end = begin + std::min<int64_t>(statement.limit, end - begin);
    }
    ExecResult result;
    result.select = true;
    result.rows.reserve(static_cast<size_t>(std::max<int64_t>(0, end - begin)));
    for (int64_t i = begin; i < end; ++i) {
      result.rows.push_back(std::move(selected[static_cast<size_t>(i)].output));
    }
    return result;
  }

  // A finite float32 value is an integer times 2^-149. Its squared distance
  // therefore has an exact integer representation in units of 2^-298. Ten
  // 64-bit words cover all float32 exponents and even a UINT32_MAX dimension.
  using ExactDistance = std::array<uint64_t, 10>;

  static void accumulateShifted(ExactDistance &sum, uint64_t value,
                                unsigned shift, bool subtract) {
    size_t word = shift / 64;
    const unsigned offset = shift % 64;
    const uint64_t low = value << offset;
    const uint64_t high = offset == 0 ? 0 : value >> (64 - offset);
    const uint64_t previous = sum[word];
    sum[word] = subtract ? previous - low : previous + low;
    uint64_t carry = high + (subtract ? previous < low : sum[word] < previous);
    while (carry != 0) {
      if (++word == sum.size()) {
        fail("VALUE_OUT_OF_RANGE", "distance accumulator overflow");
      }
      const uint64_t old = sum[word];
      sum[word] = subtract ? old - carry : old + carry;
      carry = subtract ? old < carry : sum[word] < old;
    }
  }

  static ExactDistance exactSquaredDistance(const std::vector<float> &left,
                                            const std::vector<float> &right) {
    ExactDistance sum{};
    for (size_t i = 0; i < left.size(); ++i) {
      const uint32_t a = std::bit_cast<uint32_t>(left[i]);
      const uint32_t b = std::bit_cast<uint32_t>(right[i]);
      const unsigned ae = (a >> 23) & 255U;
      const unsigned be = (b >> 23) & 255U;
      const uint64_t am = (a & 0x7fffffU) | (ae == 0 ? 0 : 0x800000U);
      const uint64_t bm = (b & 0x7fffffU) | (be == 0 ? 0 : 0x800000U);
      const unsigned as = ae == 0 ? 0 : ae - 1;
      const unsigned bs = be == 0 ? 0 : be - 1;
      accumulateShifted(sum, am * am, 2 * as, false);
      accumulateShifted(sum, bm * bm, 2 * bs, false);
      // Add the nonnegative squares before subtracting a positive cross term.
      accumulateShifted(sum, 2 * am * bm, as + bs, ((a ^ b) >> 31) == 0);
    }
    return sum;
  }

  static int compareDistances(double a, const std::vector<float> &left,
                              double b, const std::vector<float> &right,
                              const std::vector<float> &probe) {
    // Positive-term double accumulation has a bounded rounding error. Use a
    // deliberately conservative bound for either L2 or squared-L2 scores, and
    // only pay for exact integer comparison when their error intervals overlap.
    const double tolerance = (static_cast<double>(probe.size()) + 16) * 8 *
                             std::numeric_limits<double>::epsilon() * std::max(a, b);
    if (std::abs(a - b) > tolerance) {
      return a < b ? -1 : 1;
    }
    if (left == right) {
      return 0;
    }
    const ExactDistance exact_left = exactSquaredDistance(left, probe);
    const ExactDistance exact_right = exactSquaredDistance(right, probe);
    for (size_t i = exact_left.size(); i-- > 0;) {
      if (exact_left[i] != exact_right[i]) {
        return exact_left[i] < exact_right[i] ? -1 : 1;
      }
    }
    return 0;
  }

  static double l2Distance(const std::vector<float> &left,
                           const std::vector<float> &right) {
    return std::sqrt(squaredDistance(left, right));
  }

  static double squaredDistance(const std::vector<float> &left,
                                const std::vector<float> &right) {
    if (left.size() != right.size()) {
      fail("VECTOR_DIMENSION_MISMATCH", "vector dimension mismatch");
    }
#if !defined(MVD_FORCE_SCALAR) && (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
      return squaredDistanceAvx2(left.data(), right.data(), left.size());
    }
#endif
    return squaredDistanceScalar(left.data(), right.data(), left.size());
  }

#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
  __attribute__((target("avx2,fma"))) static __m256d loadProbe4(
      const float *probe) {
    return _mm256_cvtps_pd(_mm_loadu_ps(probe));
  }

  __attribute__((target("avx2,fma"))) static __m256d loadProbe4(
      const double *probe) {
    return _mm256_loadu_pd(probe);
  }

  template <typename Probe>
  __attribute__((target("avx2,fma"))) static double squaredDistanceAvx2(
      const float *left, const Probe *right, size_t size) {
    // Widen before subtracting: all finite float32 inputs, including subnormal
    // and near-FLT_MAX values, have finite, non-underflowing squares in double.
    __m256d sum0 = _mm256_setzero_pd();
    __m256d sum1 = _mm256_setzero_pd();
    __m256d sum2 = _mm256_setzero_pd();
    __m256d sum3 = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 16 <= size; i += 16) {
      const __m256d d0 = _mm256_sub_pd(
          _mm256_cvtps_pd(_mm_loadu_ps(left + i)),
          loadProbe4(right + i));
      const __m256d d1 = _mm256_sub_pd(
          _mm256_cvtps_pd(_mm_loadu_ps(left + i + 4)),
          loadProbe4(right + i + 4));
      const __m256d d2 = _mm256_sub_pd(
          _mm256_cvtps_pd(_mm_loadu_ps(left + i + 8)),
          loadProbe4(right + i + 8));
      const __m256d d3 = _mm256_sub_pd(
          _mm256_cvtps_pd(_mm_loadu_ps(left + i + 12)),
          loadProbe4(right + i + 12));
      sum0 = _mm256_fmadd_pd(d0, d0, sum0);
      sum1 = _mm256_fmadd_pd(d1, d1, sum1);
      sum2 = _mm256_fmadd_pd(d2, d2, sum2);
      sum3 = _mm256_fmadd_pd(d3, d3, sum3);
    }
    sum0 = _mm256_add_pd(sum0, sum1);
    sum2 = _mm256_add_pd(sum2, sum3);
    sum0 = _mm256_add_pd(sum0, sum2);
    alignas(32) double lanes[4];
    _mm256_store_pd(lanes, sum0);
    double sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < size; ++i) {
      const double difference = static_cast<double>(left[i]) - right[i];
      sum += difference * difference;
    }
    return static_cast<double>(sum);
  }
#endif

  template <typename Probe>
  static double squaredDistanceScalar(const float *left, const Probe *right,
                                      size_t size) {
    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    double s3 = 0.0;
    size_t i = 0;
    for (; i + 4 <= size; i += 4) {
      double d0 = static_cast<double>(left[i]) - right[i];
      double d1 = static_cast<double>(left[i + 1]) - right[i + 1];
      double d2 = static_cast<double>(left[i + 2]) - right[i + 2];
      double d3 = static_cast<double>(left[i + 3]) - right[i + 3];
      s0 += d0 * d0;
      s1 += d1 * d1;
      s2 += d2 * d2;
      s3 += d3 * d3;
    }
    double sum = s0 + s1 + s2 + s3;
    for (; i < size; ++i) {
      const double difference = static_cast<double>(left[i]) - right[i];
      sum += difference * difference;
    }
    return static_cast<double>(sum);
  }

  static int compareValues(const Value &left, const Value &right,
                           ColumnType type) {
    if (type == ColumnType::VarChar) {
      if (left.text < right.text) {
        return -1;
      }
      if (left.text > right.text) {
        return 1;
      }
      return 0;
    }
    if (type == ColumnType::Vector) {
      fail("UNSUPPORTED", "VECTOR columns cannot be ordered directly");
    }
    if (left.integer < right.integer) {
      return -1;
    }
    if (left.integer > right.integer) {
      return 1;
    }
    return 0;
  }

  fs::path data_dir_;
  bool ann_mode_ = false;
  bool explicit_transaction_ = false;
  uint32_t next_oid_ = 1;
  uint64_t next_txid_ = 1;
  std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
  std::unordered_set<uint64_t> committed_txids_;
  std::unordered_map<uint32_t, std::map<int64_t, std::optional<Row>>> overlay_;
  std::unique_ptr<FileWriter> wal_writer_;
};

enum class SplitState {
  Normal,
  SingleQuote,
  DoubleQuote,
  LineComment,
  BlockComment
};

std::vector<std::string> splitStatements(std::string_view input) {
  std::vector<std::string> statements;
  std::string current;
  SplitState state = SplitState::Normal;
  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    const char next = i + 1 < input.size() ? input[i + 1] : '\0';
    switch (state) {
      case SplitState::Normal:
        if (c == '\'') {
          state = SplitState::SingleQuote;
          current.push_back(c);
        } else if (c == '"') {
          state = SplitState::DoubleQuote;
          current.push_back(c);
        } else if (c == '-' && next == '-') {
          state = SplitState::LineComment;
          current.push_back(c);
          current.push_back(next);
          ++i;
        } else if (c == '#') {
          state = SplitState::LineComment;
          current.push_back(c);
        } else if (c == '/' && next == '*') {
          state = SplitState::BlockComment;
          current.push_back(c);
          current.push_back(next);
          ++i;
        } else if (c == ';') {
          if (!current.empty()) {
            statements.push_back(std::move(current));
            current.clear();
          }
        } else {
          current.push_back(c);
        }
        break;
      case SplitState::SingleQuote:
        current.push_back(c);
        if (c == '\'' && next == '\'') {
          current.push_back(next);
          ++i;
        } else if (c == '\'' && (i == 0 || input[i - 1] != '\\')) {
          state = SplitState::Normal;
        }
        break;
      case SplitState::DoubleQuote:
        current.push_back(c);
        if (c == '"' && next == '"') {
          current.push_back(next);
          ++i;
        } else if (c == '"' && (i == 0 || input[i - 1] != '\\')) {
          state = SplitState::Normal;
        }
        break;
      case SplitState::LineComment:
        current.push_back(c);
        if (c == '\n') {
          state = SplitState::Normal;
        }
        break;
      case SplitState::BlockComment:
        current.push_back(c);
        if (c == '*' && next == '/') {
          current.push_back(next);
          ++i;
          state = SplitState::Normal;
        }
        break;
    }
  }
  if (!current.empty()) {
    bool only_space = true;
    for (char c : current) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        only_space = false;
        break;
      }
    }
    if (!only_space) {
      statements.push_back(std::move(current));
    }
  }
  return statements;
}

void printResult(const ExecResult &result, const std::optional<std::string> &marker,
                 bool column_header) {
  if (result.select) {
    if (column_header) {
      std::cout << "COLUMNS";
      for (const std::string &column : result.columns) {
        std::cout << '\t' << column;
      }
      std::cout << '\n';
    }
    for (const auto &row : result.rows) {
      for (size_t i = 0; i < row.size(); ++i) {
        if (i != 0) {
          std::cout << '\t';
        }
        std::cout << row[i];
      }
      std::cout << '\n';
    }
  } else {
    std::cout << "OK";
    if (result.affected.has_value()) {
      std::cout << ' ' << *result.affected;
    }
    std::cout << '\n';
  }
  if (marker.has_value()) {
    std::cout << *marker << '\n';
  }
  std::cout.flush();
}

std::string numericErrorCode(const std::string &code) {
  static const std::unordered_map<std::string, int> codes = {
      {"SQL_SYNTAX", 1064},
      {"UNSUPPORTED", 1235},
      {"UNKNOWN_TABLE", 1146},
      {"TABLE_EXISTS", 1050},
      {"UNKNOWN_COLUMN", 1054},
      {"TYPE_MISMATCH", 1366},
      {"VALUE_OUT_OF_RANGE", 1264},
      {"VALUE_TOO_LONG", 1406},
      {"DUPLICATE_PRIMARY_KEY", 1062},
      {"PRIMARY_KEY_REQUIRED", 1171},
      {"VECTOR_DIMENSION_MISMATCH", 1105},
      {"TRANSACTION_ACTIVE", 1568},
      {"INVALID_UTF8", 1300},
      {"CORRUPT_DATA", 1105},
      {"IO_ERROR", 1105},
      {"INTERNAL_ERROR", 1105}};
  auto found = codes.find(code);
  return std::to_string(found == codes.end() ? 1105 : found->second);
}

void printError(const DBError &error, bool numeric_errors,
                const std::optional<std::string> &marker) {
  std::cout << "ERROR ";
  if (numeric_errors) {
    std::cout << numericErrorCode(error.code());
  } else {
    std::cout << error.code();
  }
  std::cout << ": " << error.what() << '\n';
  if (marker.has_value()) {
    std::cout << *marker << '\n';
  }
  std::cout.flush();
}

struct Options {
  fs::path data_dir = "/data";
  bool ann = false;
  bool numeric_errors = false;
  bool column_header = false;
  std::optional<std::string> marker;
  std::optional<std::string> startup_marker;
};

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    auto valueAfter = [&](std::string_view prefix) -> std::optional<std::string> {
      std::string needed(prefix);
      if (argument.rfind(needed, 0) == 0) {
        return argument.substr(needed.size());
      }
      return std::nullopt;
    };
    if (auto value = valueAfter("--data-dir=")) {
      options.data_dir = *value;
    } else if (argument == "--data-dir" && i + 1 < argc) {
      options.data_dir = argv[++i];
    } else if (auto value = valueAfter("--vector-mode=")) {
      if (*value != "exact" && *value != "ann") {
        fail("UNSUPPORTED", "--vector-mode must be exact or ann");
      }
      options.ann = *value == "ann";
    } else if (argument == "--vector-mode" && i + 1 < argc) {
      std::string mode = lower(argv[++i]);
      if (mode != "exact" && mode != "ann") {
        fail("UNSUPPORTED", "--vector-mode must be exact or ann");
      }
      options.ann = mode == "ann";
    } else if (auto value = valueAfter("--ready-marker=")) {
      options.marker = *value;
    } else if (argument == "--ready-marker" && i + 1 < argc) {
      options.marker = argv[++i];
    } else if (auto value = valueAfter("--startup-marker=")) {
      options.startup_marker = *value;
    } else if (argument == "--startup-marker" && i + 1 < argc) {
      options.startup_marker = argv[++i];
    } else if (argument == "--numeric-errors") {
      options.numeric_errors = true;
    } else if (argument == "--column-header") {
      options.column_header = true;
    } else if (argument == "--help" || argument == "-h") {
      std::cout
          << "Usage: minivectordb [--data-dir PATH] [--vector-mode exact|ann]\n"
             "       [--ready-marker TOKEN] [--startup-marker TOKEN]\n"
             "       [--numeric-errors] [--column-header]\n";
      std::exit(0);
    } else {
      fail("UNSUPPORTED", "unknown command-line argument '" + argument + "'");
    }
  }
  return options;
}

int run(int argc, char **argv) {
  Options options = parseOptions(argc, argv);
  Database database(options.data_dir, options.ann);
  std::cerr << "[minivectordb] ready data_dir=" << options.data_dir.string()
            << " mode=" << (options.ann ? "ann" : "exact") << '\n';
  if (options.startup_marker.has_value()) {
    std::cout << *options.startup_marker << '\n';
    std::cout.flush();
  }

  auto executeOne = [&](std::string sql) {
    if (sql.size() >= 3 &&
        static_cast<unsigned char>(sql[0]) == 0xefU &&
        static_cast<unsigned char>(sql[1]) == 0xbbU &&
        static_cast<unsigned char>(sql[2]) == 0xbfU) {
      sql.erase(0, 3);
    }
    bool nonspace = false;
    for (char c : sql) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        nonspace = true;
        break;
      }
    }
    if (!nonspace) {
      return;
    }
    try {
      printResult(database.execute(sql), options.marker, options.column_header);
    } catch (const DBError &error) {
      printError(error, options.numeric_errors, options.marker);
    } catch (const std::exception &error) {
      printError(DBError("INTERNAL_ERROR", error.what()), options.numeric_errors,
                 options.marker);
    }
  };

  std::string current;
  SplitState state = SplitState::Normal;
  char c = '\0';
  char next = '\0';
  while (std::cin.get(c)) {
    next = std::cin.peek();
    switch (state) {
      case SplitState::Normal:
        if (c == '\'') {
          state = SplitState::SingleQuote;
          current.push_back(c);
        } else if (c == '"') {
          state = SplitState::DoubleQuote;
          current.push_back(c);
        } else if (c == '-' && next == '-') {
          state = SplitState::LineComment;
          current.push_back(c);
        } else if (c == '#') {
          state = SplitState::LineComment;
          current.push_back(c);
        } else if (c == '/' && next == '*') {
          state = SplitState::BlockComment;
          current.push_back(c);
        } else if (c == ';') {
          executeOne(std::move(current));
          current.clear();
        } else {
          current.push_back(c);
        }
        break;
      case SplitState::SingleQuote:
        current.push_back(c);
        if (c == '\\' && std::cin.peek() != EOF) {
          current.push_back(static_cast<char>(std::cin.get()));
        } else if (c == '\'' && std::cin.peek() == '\'') {
          current.push_back(static_cast<char>(std::cin.get()));
        } else if (c == '\'') {
          state = SplitState::Normal;
        }
        break;
      case SplitState::DoubleQuote:
        current.push_back(c);
        if (c == '\\' && std::cin.peek() != EOF) {
          current.push_back(static_cast<char>(std::cin.get()));
        } else if (c == '"' && std::cin.peek() == '"') {
          current.push_back(static_cast<char>(std::cin.get()));
        } else if (c == '"') {
          state = SplitState::Normal;
        }
        break;
      case SplitState::LineComment:
        current.push_back(c);
        if (c == '\n') {
          state = SplitState::Normal;
        }
        break;
      case SplitState::BlockComment:
        current.push_back(c);
        if (c == '*' && std::cin.peek() == '/') {
          current.push_back(static_cast<char>(std::cin.get()));
          state = SplitState::Normal;
        }
        break;
    }
  }
  if (!current.empty()) {
    executeOne(std::move(current));
  }
  return 0;
}

}  // namespace mvd

int main(int argc, char **argv) {
  // Responses explicitly flush stdout. Avoid flushing it, and synchronizing
  // with C stdio, for every get()/peek() while reading a SQL request.
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  try {
    return mvd::run(argc, argv);
  } catch (const mvd::DBError &error) {
    std::cerr << "ERROR " << error.code() << ": " << error.what() << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "ERROR INTERNAL_ERROR: " << error.what() << '\n';
    return 1;
  }
}
