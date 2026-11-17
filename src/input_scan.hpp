#ifndef MVD_INPUT_SCAN_FINAL_HPP
#define MVD_INPUT_SCAN_FINAL_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if !defined(MVD_FORCE_SCALAR) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define MVD_INPUT_SCAN_FINAL_AVX2 1
#else
#define MVD_INPUT_SCAN_FINAL_AVX2 0
#endif

namespace mvd::input_scan {

// Return the first quote/backslash, or n. The input exposes exactly n readable
// bytes; no terminator, alignment, padding, or additional lookahead is needed.
// As in the original lexer, doubled quotes and escapes stay with the decoder.
inline std::size_t sql_scalar(const char *s, std::size_t n,
                               char quote) noexcept {
  constexpr std::uint64_t ones = 0x0101010101010101ULL;
  constexpr std::uint64_t high = 0x8080808080808080ULL;
  const std::uint64_t quotes = ones * static_cast<unsigned char>(quote);
  constexpr std::uint64_t slashes = ones * static_cast<unsigned char>('\\');
  std::size_t i = 0;
  for (; n - i >= sizeof(std::uint64_t); i += sizeof(std::uint64_t)) {
    std::uint64_t word;
    std::memcpy(&word, s + i, sizeof(word));
    const std::uint64_t q = word ^ quotes;
    const std::uint64_t b = word ^ slashes;
    // Borrow propagation permits an existence test, not byte-lane locations.
    if ((((q - ones) & ~q) | ((b - ones) & ~b)) & high) break;
  }
  while (i < n && s[i] != quote && s[i] != '\\') ++i;
  return i;
}

// Return the first quote, backslash, control byte, or non-ASCII byte, or n.
// This skips ordinary ASCII only. Every UTF-8 byte, including malformed input,
// must reach the existing decoder and its strict UTF-8 validation unchanged.
inline std::size_t json_scalar(const char *s, std::size_t n) noexcept {
  constexpr std::uint64_t ones = 0x0101010101010101ULL;
  constexpr std::uint64_t high = 0x8080808080808080ULL;
  const auto has_zero_byte = [](std::uint64_t word) noexcept {
    return ((word - ones) & ~word & high) != 0;
  };
  std::size_t i = 0;
  for (; n - i >= sizeof(std::uint64_t); i += sizeof(std::uint64_t)) {
    std::uint64_t word;
    std::memcpy(&word, s + i, sizeof(word));
    // These original SWAR masks answer only whether the block has a special
    // byte. Do not countr_zero this mask: subtraction can propagate borrows.
    if ((word & high) != 0 ||
        ((word - 0x2020202020202020ULL) & ~word & high) != 0 ||
        has_zero_byte(word ^ 0x2222222222222222ULL) ||
        has_zero_byte(word ^ 0x5c5c5c5c5c5c5c5cULL)) {
      break;
    }
  }
  for (; i < n; ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x20U || c >= 0x80U || c == '"' || c == '\\') break;
  }
  return i;
}

// Scan bytes after '[' without validating numeric grammar. Stop at the first
// byte outside the numeric-array alphabet, or the first '-' of a SQL '--'
// comment. A lone final '-' is allowed; no byte beyond n is inspected.
inline std::size_t numeric_array_prefix_scalar(const char *s,
                                              std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    const bool allowed = (c >= '0' && c <= '9') || c == 'e' || c == 'E' ||
        (c >= '+' && c <= '.') || c == ' ' || (c >= '\t' && c <= '\r');
    if (!allowed || (c == '-' && n - i > 1 && s[i + 1] == '-')) return i;
  }
  return n;
}

#if MVD_INPUT_SCAN_FINAL_AVX2
// Call through the dispatch helpers, or explicitly check real CPU support.
__attribute__((target("avx2"))) inline std::size_t sql_avx2(
    const char *s, std::size_t n, char quote) noexcept {
  const __m256i q = _mm256_set1_epi8(quote);
  const __m256i slash = _mm256_set1_epi8('\\');
  std::size_t i = 0;
  for (; n - i >= 32; i += 32) {
    const __m256i bytes =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(s + i));
    const __m256i special = _mm256_or_si256(
        _mm256_cmpeq_epi8(bytes, q), _mm256_cmpeq_epi8(bytes, slash));
    const auto mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(special));
    if (mask != 0) return i + std::countr_zero(mask);
  }
  return i + sql_scalar(s + i, n - i, quote);
}

__attribute__((target("avx2"))) inline std::size_t json_avx2(
    const char *s, std::size_t n) noexcept {
  const __m256i q = _mm256_set1_epi8('"');
  const __m256i slash = _mm256_set1_epi8('\\');
  const __m256i space = _mm256_set1_epi8(0x20);
  std::size_t i = 0;
  for (; n - i >= 32; i += 32) {
    const __m256i bytes =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(s + i));
    // Signed comparison also catches every byte >= 0x80 as a negative lane.
    const __m256i special = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(bytes, q),
                         _mm256_cmpeq_epi8(bytes, slash)),
        _mm256_cmpgt_epi8(space, bytes));
    const auto mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(special));
    if (mask != 0) return i + std::countr_zero(mask);
  }
  return i + json_scalar(s + i, n - i);
}

__attribute__((target("avx2"))) inline std::size_t numeric_array_prefix_avx2(
    const char *s, std::size_t n) noexcept {
  const __m256i digit_low = _mm256_set1_epi8('0' - 1);
  const __m256i digit_high = _mm256_set1_epi8('9' + 1);
  const __m256i punctuation_low = _mm256_set1_epi8('+' - 1);
  const __m256i punctuation_high = _mm256_set1_epi8('.' + 1);
  const __m256i whitespace_low = _mm256_set1_epi8('\t' - 1);
  const __m256i whitespace_high = _mm256_set1_epi8('\r' + 1);
  const __m256i space = _mm256_set1_epi8(' ');
  const __m256i lowercase_bit = _mm256_set1_epi8(0x20);
  const __m256i exponent = _mm256_set1_epi8('e');
  const __m256i minus = _mm256_set1_epi8('-');
  std::size_t i = 0;
  for (; n - i >= 32; i += 32) {
    const __m256i bytes =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(s + i));
    // Signed range checks reject all non-ASCII bytes at their lower bound.
    const __m256i digits = _mm256_and_si256(
        _mm256_cmpgt_epi8(bytes, digit_low),
        _mm256_cmpgt_epi8(digit_high, bytes));
    // '+', ',', '-', '.' are consecutive ASCII bytes.
    const __m256i punctuation = _mm256_and_si256(
        _mm256_cmpgt_epi8(bytes, punctuation_low),
        _mm256_cmpgt_epi8(punctuation_high, bytes));
    const __m256i whitespace = _mm256_or_si256(
        _mm256_cmpeq_epi8(bytes, space),
        _mm256_and_si256(_mm256_cmpgt_epi8(bytes, whitespace_low),
                         _mm256_cmpgt_epi8(whitespace_high, bytes)));
    const __m256i exponents = _mm256_cmpeq_epi8(
        _mm256_or_si256(bytes, lowercase_bit), exponent);
    const __m256i allowed = _mm256_or_si256(
        _mm256_or_si256(digits, punctuation),
        _mm256_or_si256(whitespace, exponents));
    const auto allowed_mask =
        static_cast<std::uint32_t>(_mm256_movemask_epi8(allowed));
    const auto minus_mask = static_cast<std::uint32_t>(
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, minus)));
    std::uint32_t special = ~allowed_mask | (minus_mask & (minus_mask >> 1));
    // Include a pair starting in lane 31, even when the next byte is in the
    // scalar tail. The explicit length check keeps the lookahead in bounds.
    if ((minus_mask & 0x80000000U) != 0 && n - i > 32 && s[i + 32] == '-') {
      special |= 0x80000000U;
    }
    if (special != 0) return i + std::countr_zero(special);
  }
  if (i == n) return n;
  return i + numeric_array_prefix_scalar(s + i, n - i);
}
#endif

inline std::size_t sql_special(const char *s, std::size_t n,
                                char quote) noexcept {
#if MVD_INPUT_SCAN_FINAL_AVX2
  if (n >= 32 && __builtin_cpu_supports("avx2")) return sql_avx2(s, n, quote);
#endif
  return sql_scalar(s, n, quote);
}

inline std::size_t json_special(const char *s, std::size_t n) noexcept {
#if MVD_INPUT_SCAN_FINAL_AVX2
  if (n >= 32 && __builtin_cpu_supports("avx2")) return json_avx2(s, n);
#endif
  return json_scalar(s, n);
}

inline std::size_t numeric_array_prefix(const char *s, std::size_t n) noexcept {
#if MVD_INPUT_SCAN_FINAL_AVX2
  if (n >= 32 && __builtin_cpu_supports("avx2")) {
    return numeric_array_prefix_avx2(s, n);
  }
#endif
  return numeric_array_prefix_scalar(s, n);
}

}  // namespace mvd::input_scan

#undef MVD_INPUT_SCAN_FINAL_AVX2
#endif  // MVD_INPUT_SCAN_FINAL_HPP
