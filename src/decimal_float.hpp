#ifndef MVD_DECIMAL_FLOAT_HPP
#define MVD_DECIMAL_FLOAT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mvd {

// A decimal with at most nine digits and at most nine fractional digits has
// denominator 2^d*5^d, d<=9. Unless exactly a binary32 midpoint, its distance
// from a midpoint is at least 1/(2*5^9) binary32 ULP, over 100 binary64 ULPs.
// The exact integer and divisor fit binary64. One correctly rounded division
// followed by conversion to binary32 therefore agrees with direct rounding.
// An exact midpoint has its factors of five cancelled and is exact in double.
inline float shortDecimalFloat32(uint32_t magnitude, size_t fractional,
                                 bool negative) noexcept {
  constexpr std::array<double, 10> divisor{
      1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
  const float result = static_cast<float>(static_cast<double>(magnitude) /
                                           divisor[fractional]);
  return negative ? -result : result;
}

inline bool parseShortDecimalFloat32(std::string_view text, float &value) noexcept {
  size_t pos = 0;
  bool negative = false;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    negative = text[pos++] == '-';
  }
  uint32_t magnitude = 0;
  size_t digits = 0;
  const auto digit = [&] { return pos < text.size() &&
      text[pos] >= '0' && text[pos] <= '9'; };
  while (digit()) {
    if (++digits > 9) return false;
    magnitude = magnitude * 10 + static_cast<unsigned>(text[pos++] - '0');
  }
  if (digits == 0) return false;
  size_t fractional = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    while (digit()) {
      if (++digits > 9) return false;
      magnitude = magnitude * 10 + static_cast<unsigned>(text[pos++] - '0');
      ++fractional;
    }
  }
  if (pos != text.size()) return false;
  value = shortDecimalFloat32(magnitude, fractional, negative);
  return true;
}

}  // namespace mvd
#endif
