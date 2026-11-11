#include "../src/decimal_float.hpp"
#include <bit>
#include <charconv>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
size_t checked = 0;
void check(const std::string &text) {
  float actual = 123;
  if (!mvd::parseShortDecimalFloat32(text, actual)) throw std::runtime_error("unexpected fallback " + text);
  float expected = 0;
  const char *start = text.data() + (text.front() == '+');
  auto result = std::from_chars(start, text.data() + text.size(), expected);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
      std::bit_cast<uint32_t>(actual) != std::bit_cast<uint32_t>(expected)) {
    throw std::runtime_error("float32 mismatch " + text);
  }
  ++checked;
}
}

int main() {
  try {
    for (uint32_t i = 0; i < 1000000; ++i) {
      std::string fraction = std::to_string(i + 1000000).substr(1);
      check("0." + fraction);
      check("-0." + fraction);
    }
    std::mt19937 rng(20261111);
    for (size_t i = 0; i < 300000; ++i) {
      std::string s = std::to_string(rng() % 1000000000);
      size_t dot = rng() % (s.size() + 1);
      if (dot == 0) s = "0." + s.substr(0, 8);
      else s.insert(dot, ".");
      check(s);
      check("-" + s);
      check("+" + s);
    }
    for (const char *s : {"0", "-0", "+0.", "-0.00000000", "16777217", "16777219",
                         "999999999", "0.00000001", "0.5", "125.125", "1.", "+0000"}) check(s);
    for (const char *s : {"", "-", "+", ".5", "1e3", "1e-400", "1e999", "nan", "inf",
                         "1.000000000", "1000000000", "1x", "1..", " 1", "1 ", "--1"}) {
      float value = 99;
      if (mvd::parseShortDecimalFloat32(s, value) || value != 99) {
        throw std::runtime_error("invalid or long value did not fall back");
      }
    }
    std::cout << "PASS " << checked << " independent decimal-to-float32 bit comparisons and fallbacks\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
