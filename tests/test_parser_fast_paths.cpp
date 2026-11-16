// Parser-only differential checks: no Database, files, child processes or timing.
// The scalar reference preserves the pre-fast-path grammar and error ordering;
// numeric conversion uses independent std::from_chars, including short numbers.
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
size_t vectors_checked = 0, strings_checked = 0;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// Frozen scalar range-error classification, independent of the fused scanner.
bool scalarMagnitudeBelowOne(std::string_view text) {
  if (!text.empty() && (text.front() == '-' || text.front() == '+'))
    text.remove_prefix(1);
  const size_t e = text.find_first_of("eE");
  const auto significand = text.substr(0, e);
  const size_t point = significand.find('.');
  const size_t decimal_position =
      point == std::string_view::npos ? significand.size() : point;
  const size_t first = significand.find_first_of("123456789");
  if (first == std::string_view::npos) return true;
  const size_t first_digit = first - (first > decimal_position ? 1 : 0);
  const int64_t threshold = static_cast<int64_t>(first_digit + 1) -
                            static_cast<int64_t>(decimal_position);
  if (e == std::string_view::npos) return 0 < threshold;
  auto exponent_text = text.substr(e + 1);
  if (!exponent_text.empty() && exponent_text.front() == '+')
    exponent_text.remove_prefix(1);
  int64_t exponent = 0;
  const auto parsed = std::from_chars(exponent_text.data(),
      exponent_text.data() + exponent_text.size(), exponent);
  if (parsed.ec == std::errc::result_out_of_range)
    return !exponent_text.empty() && exponent_text.front() == '-';
  return parsed.ec == std::errc() &&
         parsed.ptr == exponent_text.data() + exponent_text.size() &&
         exponent < threshold;
}

float scalarFloat(std::string_view text) {
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  float value = 0;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec == std::errc::result_out_of_range &&
      parsed.ptr == text.data() + text.size() && scalarMagnitudeBelowOne(text)) {
    value = text.front() == '-' ? -0.0f : 0.0f;
    parsed.ec = std::errc();
  }
  if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
      !std::isfinite(value))
    mvd::fail("VALUE_OUT_OF_RANGE", "vector component is not a finite float32");
  return value;
}

std::vector<float> scalarVector(std::string_view text, size_t dimension) {
  size_t pos = 0;
  const auto space = [&] {
    while (pos < text.size() &&
           std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  };
  space();
  if (pos == text.size() || text[pos++] != '[')
    mvd::fail("TYPE_MISMATCH", "VECTOR requires a bracketed numeric array");
  std::vector<float> values;
  space();
  if (pos < text.size() && text[pos] == ']') {
    ++pos;
  } else {
    while (true) {
      space();
      const size_t start = pos;
      if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
      const size_t integer_start = pos;
      while (pos < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
      if (pos == integer_start)
        mvd::fail("TYPE_MISMATCH", "vector component must be numeric");
      if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
      }
      if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
        const size_t exponent_start = pos;
        while (pos < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
        if (pos == exponent_start)
          mvd::fail("SQL_SYNTAX", "numeric exponent requires digits");
      }
      values.push_back(scalarFloat(text.substr(start, pos - start)));
      space();
      if (pos < text.size() && text[pos] == ']') {
        ++pos;
        break;
      }
      if (pos == text.size() || text[pos++] != ',')
        mvd::fail("SQL_SYNTAX", "expected ',' or ']' in vector literal");
    }
  }
  space();
  if (pos < text.size() && text[pos] == ';') {
    ++pos;
    space();
  }
  if (pos != text.size())
    mvd::fail("UNSUPPORTED", "unexpected characters after vector literal");
  if (values.size() != dimension)
    mvd::fail("VECTOR_DIMENSION_MISMATCH",
              "query vector dimension does not match schema");
  return values;
}

struct VectorOutcome {
  std::vector<uint32_t> bits;
  std::string code, message;
  bool operator==(const VectorOutcome &) const = default;
};

template <class Parse>
VectorOutcome captureVector(Parse parse) {
  VectorOutcome result;
  try {
    for (float value : parse()) result.bits.push_back(std::bit_cast<uint32_t>(value));
  } catch (const mvd::DBError &error) {
    result.code = error.code();
    result.message = error.what();
  }
  return result;
}

void checkVector(std::string_view text, size_t dimension) {
  const auto expected = captureVector([&] { return scalarVector(text, dimension); });
  const auto actual = captureVector([&] {
    return mvd::Parser::bindProbeLiteral(text, dimension);
  });
  if (actual != expected)
    throw std::runtime_error("vector bits/error precedence changed: " +
                             std::string(text.substr(0, 120)));
  ++vectors_checked;
}

struct Quoted {
  std::string text;
  size_t next = 0;
  bool decoded = false;
};

// Scalar SQL quote decoder: no word masks or production scanning helper.
Quoted scalarQuoted(std::string_view input, size_t pos) {
  const char quote = input[pos++];
  Quoted result;
  while (pos < input.size()) {
    char c = input[pos++];
    if (c == quote) {
      if (pos < input.size() && input[pos] == quote) {
        ++pos;
        result.decoded = true;
      } else {
        result.next = pos;
        return result;
      }
    } else if (c == '\\' && pos < input.size()) {
      c = input[pos++];
      result.decoded = true;
      if (c == 'n') c = '\n';
      else if (c == 'r') c = '\r';
      else if (c == 't') c = '\t';
    }
    result.text.push_back(c);
  }
  mvd::fail("SQL_SYNTAX", "unterminated string literal");
}

void checkQuoted(const std::string &token, size_t alignment, bool suffix = true) {
  const std::string frame = std::string(alignment, ' ') + token +
                            (suffix ? ",17" : "");
  // No terminator/slack after the input view: sanitizers can detect tail loads.
  auto exact = std::make_unique<char[]>(frame.size());
  std::memcpy(exact.get(), frame.data(), frame.size());
  const std::string_view input(exact.get(), frame.size());
  std::optional<Quoted> expected;
  std::string expected_code, expected_message;
  try {
    expected = scalarQuoted(input, alignment);
  } catch (const mvd::DBError &error) {
    expected_code = error.code();
    expected_message = error.what();
  }
  std::vector<std::unique_ptr<std::string>> decoded;
  std::vector<mvd::Token> tokens;
  try {
    tokens = mvd::Lexer(input, decoded).tokenize();
  } catch (const mvd::DBError &error) {
    require(!expected && error.code() == expected_code &&
                error.what() == expected_message,
            "quoted string error changed");
    ++strings_checked;
    return;
  }
  require(expected.has_value(), "unterminated quoted string was accepted");
  require(expected->next == alignment + token.size(), "invalid quote fixture");
  require(tokens.size() == (suffix ? 4U : 2U) &&
              tokens[0].kind == mvd::TokenKind::String &&
              tokens[0].text == expected->text && tokens[0].position == alignment &&
              tokens.back().kind == mvd::TokenKind::End &&
              tokens.back().position == input.size(),
          "quoted bytes, positions or trailing tokens changed");
  require(decoded.size() == (expected->decoded ? 1U : 0U),
          "plain/escaped literal ownership changed");
  if (!expected->decoded)
    require(tokens[0].text.data() == input.data() + alignment + 1,
            "ordinary literal stopped borrowing its exact input span");
  if (suffix)
    require(tokens[1].kind == mvd::TokenKind::Symbol && tokens[1].text == "," &&
                tokens[1].position == expected->next &&
                tokens[2].kind == mvd::TokenKind::Number && tokens[2].text == "17" &&
                tokens[2].position == expected->next + 1,
            "quoted string consumed its following delimiter");
  ++strings_checked;
}

void checkStringBoundaries() {
  for (char quote : {'\'', '"'}) {
    for (size_t alignment = 0; alignment < 8; ++alignment) {
      // All byte values in all word lanes and unaligned input positions.
      for (size_t lane = 0; lane < 8; ++lane) {
        for (unsigned byte = 0; byte < 256; ++byte) {
          std::string token = std::string(1, quote) + std::string(16 + lane, 'A');
          if (byte == static_cast<unsigned char>(quote)) token.append(2, quote);
          else if (byte == '\\') token += "\\\\";
          else token += static_cast<char>(byte);
          token += std::string(17, 'B') + quote;
          checkQuoted(token, alignment);
        }
      }
      std::vector<size_t> lengths;
      for (size_t length = 0; length <= 65; ++length) lengths.push_back(length);
      for (size_t length : {127U, 128U, 129U, 1153U, 4095U, 4096U, 4097U})
        lengths.push_back(length);
      for (size_t length : lengths) {
        const std::string prefix = std::string(1, quote) + std::string(length, 'p');
        checkQuoted(prefix + quote, alignment, false);
        checkQuoted(prefix, alignment, false);
        checkQuoted(prefix + "\\", alignment, false);
        checkQuoted(prefix + std::string(2, quote), alignment, false);
        for (const auto &escape : std::vector<std::string>{
                 std::string(2, quote), std::string("\\") + quote,
                 "\\n", "\\r", "\\t", "\\q", "\\\\", "\\0"})
          checkQuoted(prefix + escape + "tail" + quote, alignment);
        checkQuoted(prefix + std::string(3, quote), alignment, false);
        checkQuoted(prefix + "\\\\" + quote, alignment, false);
      }
    }
  }
}

void checkNumericBoundaries() {
  // Cross the 9/10-digit limit with every sign/dot position and exponent form.
  for (const std::string digits : {"0", "1", "9", "000000000", "0000000000",
                                  "12345678", "123456789", "1234567890",
                                  "16777217", "16777219", "999999999"}) {
    std::vector<std::string> forms{digits};
    for (size_t point = 0; point <= digits.size(); ++point) {
      std::string dotted = digits;
      dotted.insert(point, ".");
      forms.push_back(std::move(dotted));
    }
    for (const auto &form : forms) {
      for (std::string_view sign : {"", "+", "-"}) {
        for (std::string_view exponent : {"", "e0", "E+1", "e-1", "e", "e+",
                                         "E-", "e999", "e-999",
                                         "e999999999999999999999",
                                         "e-999999999999999999999"}) {
          const std::string number = std::string(sign) + form + std::string(exponent);
          checkVector("[0.125," + number + ",-0]", 3);
        }
      }
    }
  }
  for (std::string_view number : {
           "", "+", "-", ".5", "+.5", "-.5", "++1", "--1", "+-1", "-+1",
           "1..", "1.2.3", "1e++1", "1e--1", "1e+-1", "1e-+1",
           "1e1e", "1e999e", "1e-999e", "NaN", "Infinity", "-Infinity",
           "0x1", "1_0", "1 +2", "3.4028234663852886e38", "3.5e38",
           "1.401298464324817e-45", "-1.401298464324817e-45",
           "1.000000059604644775390625", "1.0000000596046448",
           "0.000000001", "0.000000000", "-0.00000000"}) {
    checkVector("[0.125," + std::string(number) + ",-0]", 3);
  }
  for (std::string_view literal : {"", "[", "[]", "[];", "[ ] ; \t",
                                   "[1,]", "[1 2]", "[1];;", "[1]x",
                                   "[1];x", "[1,2", "[1] /*x*/"})
    for (size_t dimension : {0U, 1U, 2U}) checkVector(literal, dimension);
  const std::string complete =
      " \t[-0.000000,+1.,123456789,1e-999,3.4028234663852886e38] ; \n";
  for (size_t end = 0; end <= complete.size(); ++end)
    checkVector(std::string_view(complete).substr(0, end), 5);

  // Deterministic multi-component cases, including the default 128D width.
  uint32_t state = 0x9e3779b9U;
  for (size_t batch = 0; batch < 64; ++batch) {
    std::string vector = "[";
    for (size_t component = 0; component < 128; ++component) {
      state = state * 1664525U + 1013904223U;
      std::string number = std::to_string(state % 1000000000U);
      const size_t point = (state >> 16U) % (number.size() + 1);
      number.insert(point, ".");
      if (point == 0) number.insert(0, "0");
      if (component % 3 == 1) number.insert(0, "-");
      if (component % 3 == 2) number.insert(0, "+");
      if (component % 7 == 0) number += "e-3";
      if (component != 0) vector += ",";
      vector += " \t" + number + "\n";
    }
    checkVector(vector + "] ; ", 128);
  }
}

std::string sqlQuoted(std::string_view text, char quote) {
  std::string result(1, quote);
  for (char c : text) {
    if (c == quote) result.append(2, quote);
    else if (c == '\\') result += "\\\\";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else result += c;
  }
  return result + quote;
}

void checkStatementOwnership() {
  mvd::TableDef table;
  table.name = "t";
  table.columns = {{"id", mvd::ColumnType::BigInt, 0},
                   {"note", mvd::ColumnType::VarChar, 4096},
                   {"v", mvd::ColumnType::Vector, 4}};
  table.vector_column = 2;
  const std::unordered_map<std::string, const mvd::TableDef *> tables{{"t", &table}};
  const std::string vector = "[-0.000000,+1.,123456789,1e-999]";
  const auto expected = scalarVector(vector, 4);
  std::vector<std::string> notes;
  std::string sql = "INSERT INTO t VALUES";
  for (size_t row = 0; row < 100; ++row) {
    std::string note = std::string(row % 2 ? 129 : 1, 'a') + std::to_string(row);
    if (row % 3) note += "'\"\\\n\r\t;\xc3\xa9";
    notes.push_back(note);
    if (row != 0) sql += ",";
    const char quote = row % 2 ? '"' : '\'';
    const std::string value = row % 3 == 0 ? vector :
        sqlQuoted(row % 3 == 1 ? "\t" + vector + " ;\n" : vector, quote);
    sql += "(" + std::to_string(row) + "," + sqlQuoted(note, quote) + "," + value + ")";
  }
  mvd::InsertStatement statement;
  {
    mvd::Parser parser(sql, tables);
    statement = std::get<mvd::InsertStatement>(parser.parse());
    std::fill(sql.begin(), sql.end(), '!');
  }
  require(statement.rows.size() == notes.size(), "parsed INSERT lost rows");
  for (size_t row = 0; row < notes.size(); ++row) {
    require(statement.rows[row][0].integer() == static_cast<int64_t>(row) &&
                statement.rows[row][1].text() == notes[row] &&
                statement.rows[row][2].vector().size() == expected.size(),
            "statement borrowed poisoned SQL/decoded literal storage");
    for (size_t component = 0; component < expected.size(); ++component)
      require(std::bit_cast<uint32_t>(statement.rows[row][2].vector()[component]) ==
                  std::bit_cast<uint32_t>(expected[component]),
              "direct/quoted/escaped INSERT vector bits changed");
  }
}
}  // namespace

int main() {
  try {
    checkStringBoundaries();
    checkNumericBoundaries();
    checkStatementOwnership();
    std::cout << "PASS " << strings_checked << " quoted strings and "
              << vectors_checked << " vector literals: scalar equivalence, "
                 "float32 bits, errors, word boundaries and ownership\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
