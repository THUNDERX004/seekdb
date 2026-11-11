// Check the public JSON parser against byte/Unicode fixtures, independently of
// the word masks used to accelerate the common unescaped ASCII prefix.
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
size_t checked = 0;

void check(const std::string &token, const std::optional<std::string> &expected) {
  const std::string frame = "{\"id\":1,\"op\":\"execute\",\"sql\":" + token + "}";
  mvd::JsonRequest request;
  bool accepted = false;
  try {
    mvd::JsonRequestParser(frame).parse(request);
    accepted = true;
  } catch (const mvd::DBError &) {
  }
  if (accepted != expected.has_value() ||
      (accepted && (request.sql != *expected || request.id != "1" ||
                    request.op != "execute" || request.mode != "exact"))) {
    throw std::runtime_error("JSON ASCII boundary changed acceptance or decoded bytes");
  }
  ++checked;
}
}  // namespace

int main() {
  try {
    // Move every possible byte through all word lanes and both short tails and
    // long prefixes. A lone high byte cannot be a complete UTF-8 code point.
    for (size_t length = 0; length <= 65; ++length) {
      const std::string prefix(length, 'A');
      for (unsigned byte = 0; byte != 256; ++byte) {
        const std::string value = prefix + static_cast<char>(byte) +
                                  std::string(23, 'B');
        const bool ordinary = byte >= 0x20 && byte < 0x80 &&
                              byte != '"' && byte != '\\';
        check('"' + value + '"', ordinary
            ? std::optional<std::string>(value) : std::nullopt);
      }
      check('"' + prefix + '"', prefix);
      // A closing brace belongs to the malformed string when its quote is
      // missing. No bounded word load may read beyond that truncated frame.
      check('"' + prefix, std::nullopt);
      for (const auto &[encoded, decoded] :
           std::vector<std::pair<std::string, std::string>>{
               {"\\\"", "\""}, {"\\\\", "\\"}, {"\\/", "/"},
               {"\\b\\f\\n\\r\\t", "\b\f\n\r\t"},
               {"\\u0000", std::string(1, '\0')},
               {"\\u00e9", "\xc3\xa9"},
               {"\\ud83c\\udf0a", "\xf0\x9f\x8c\x8a"},
               {"\xc3\xa9\xe4\xb8\xad\xf0\x9f\x8c\x8a",
                "\xc3\xa9\xe4\xb8\xad\xf0\x9f\x8c\x8a"}}) {
        check('"' + prefix + encoded + "tail\"", prefix + decoded + "tail");
      }
      for (const std::string bad : {"\\x", "\\u", "\\u12", "\\ud800",
                                     "\\udc00", "\\ud800\\u0000",
                                     "\xc0\xaf", "\xed\xa0\x80",
                                     "\xf4\x90\x80\x80", "\\"}) {
        check('"' + prefix + bad + '"', std::nullopt);
      }
    }
    for (size_t length : {size_t{4095}, size_t{4096}, size_t{4097}, size_t{1048576}}) {
      const std::string plain(length, 'x');
      check('"' + plain + '"', plain);
      check('"' + plain, std::nullopt);
    }
    std::cout << "PASS " << checked
              << " public-parser byte, word-lane, escape, UTF-8 and truncation fixtures\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
