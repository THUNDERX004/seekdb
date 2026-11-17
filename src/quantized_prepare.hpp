#ifndef MVD_QUANTIZED_PREPARE_HPP
#define MVD_QUANTIZED_PREPARE_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#if !defined(MVD_FORCE_SCALAR) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
#define MVD_QUANTIZED_PREPARE_AVX2 1
#include <immintrin.h>
#include <cpuid.h>
#else
#define MVD_QUANTIZED_PREPARE_AVX2 0
#endif

namespace mvd::quantized_prepare {

inline bool available() noexcept {
#if MVD_QUANTIZED_PREPARE_AVX2
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}

#if MVD_QUANTIZED_PREPARE_AVX2

// Inputs are finite binary32 values. Min/max introduce no rounding, and the
// extrema are widened to double only after the reduction.
template <typename Rows>
__attribute__((target("avx2"))) inline std::pair<double, double> bounds(
    const Rows &rows, std::size_t dimension) noexcept {
  __m256 minimum = _mm256_set1_ps(std::numeric_limits<float>::infinity());
  __m256 maximum = _mm256_set1_ps(-std::numeric_limits<float>::infinity());
  float tail_minimum = std::numeric_limits<float>::infinity();
  float tail_maximum = -std::numeric_limits<float>::infinity();
  for (const auto &row : rows) {
    std::size_t axis = 0;
    for (; axis + 8 <= dimension; axis += 8) {
      const __m256 value = _mm256_loadu_ps(row.vector_data + axis);
      minimum = _mm256_min_ps(minimum, value);
      maximum = _mm256_max_ps(maximum, value);
    }
    for (; axis < dimension; ++axis) {
      tail_minimum = std::min(tail_minimum, row.vector_data[axis]);
      tail_maximum = std::max(tail_maximum, row.vector_data[axis]);
    }
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, minimum);
  for (float value : lanes) tail_minimum = std::min(tail_minimum, value);
  _mm256_store_ps(lanes, maximum);
  for (float value : lanes) tail_maximum = std::max(tail_maximum, value);
  return {tail_minimum, tail_maximum};
}

template <typename Rows>
__attribute__((target("avx2"))) inline double maximum_absolute(
    const Rows &rows, std::size_t dimension) noexcept {
  if (rows.empty() || dimension == 0) return 0;
  const auto [minimum, maximum] = bounds(rows, dimension);
  return std::max(std::abs(minimum), std::abs(maximum));
}

// Scale is a power of two. The caller establishes finite centered coordinates
// with |scaled| < 128. Widening/multiplication are exact; center subtraction
// may round, so the caller also retains an outward translation-error guard.
// Add-half/truncate and std::round can disagree only within a double ULP of a
// half step; either encoding retains its measured residual error.
// Only the order of positive residual-square additions changes. The caller
// applies its conservative double-precision error envelope to this maximum.
template <typename Rows>
__attribute__((target("avx2"))) inline double encode_avx2(
    const Rows &rows, std::size_t dimension, double scale, std::size_t stride,
    bool blocked, std::int8_t *codes, std::uint32_t *norms,
    double center = 0) noexcept {
  const __m256d multiplier = _mm256_set1_pd(scale);
  const __m256d origin = _mm256_set1_pd(center);
  const __m256d sign = _mm256_set1_pd(-0.0);
  const __m256d half = _mm256_set1_pd(0.5);
  const __m128i lower = _mm_set1_epi32(-127);
  const __m128i upper = _mm_set1_epi32(127);
  const __m128i offset = _mm_set1_epi8(-128);
  double maximum_error = 0;
  std::size_t index = 0;
  for (const auto &row : rows) {
    __m256d error_sums = _mm256_setzero_pd();
    __m128i norm_sums = _mm_setzero_si128();
    const std::size_t base = blocked
        ? index / 16 * 16 * stride + index % 16 * 4 : index * stride;
    std::size_t axis = 0;
    for (; axis + 4 <= dimension; axis += 4) {
      const __m256d scaled = _mm256_mul_pd(
          _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(row.vector_data + axis)),
                        origin), multiplier);
      const __m256d adjustment = _mm256_or_pd(half, _mm256_and_pd(sign, scaled));
      const __m128i code = _mm_min_epi32(upper, _mm_max_epi32(lower,
          _mm256_cvttpd_epi32(_mm256_add_pd(scaled, adjustment))));
      norm_sums = _mm_add_epi32(norm_sums, _mm_mullo_epi32(code, code));
      const __m256d error = _mm256_sub_pd(scaled, _mm256_cvtepi32_pd(code));
      error_sums = _mm256_add_pd(error_sums, _mm256_mul_pd(error, error));
      __m128i bytes = _mm_packs_epi16(_mm_packs_epi32(code, code),
                                     _mm_packs_epi32(code, code));
      if (blocked) bytes = _mm_xor_si128(bytes, offset);
      const std::uint32_t word = static_cast<std::uint32_t>(_mm_cvtsi128_si32(bytes));
      std::memcpy(codes + base + (blocked ? axis * 16 : axis), &word, sizeof(word));
    }
    alignas(32) double error_lanes[4];
    alignas(16) std::uint32_t norm_lanes[4];
    _mm256_store_pd(error_lanes, error_sums);
    _mm_store_si128(reinterpret_cast<__m128i *>(norm_lanes), norm_sums);
    double error_sum = (error_lanes[0] + error_lanes[1]) +
                       (error_lanes[2] + error_lanes[3]);
    std::uint32_t norm = norm_lanes[0] + norm_lanes[1] +
                         norm_lanes[2] + norm_lanes[3];
    for (; axis < dimension; ++axis) {
      const double scaled =
          (static_cast<double>(row.vector_data[axis]) - center) * scale;
      const int code = std::clamp(static_cast<int>(
          scaled + std::copysign(0.5, scaled)), -127, 127);
      const std::size_t position = blocked
          ? base + axis / 4 * 64 + axis % 4 : base + axis;
      const auto byte = static_cast<std::uint8_t>(code + (blocked ? 128 : 0));
      std::memcpy(codes + position, &byte, sizeof(byte));
      norm += static_cast<std::uint32_t>(code * code);
      const double error = scaled - code;
      error_sum += error * error;
    }
    norms[index++] = norm;
    maximum_error = std::max(maximum_error, error_sum);
  }
  return maximum_error;
}

// Runtime dispatch remains outside ISA-targeted functions. XGETBV is itself
// gated by XSAVE/OSXSAVE/AVX; XCR0 must enable SSE, YMM, opmask and both ZMM states.
inline bool avx512_available() noexcept {
  static const bool supported = [] {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    constexpr unsigned int required = (1U << 26) | (1U << 27) | (1U << 28);
    if ((ecx & required) != required) return false;
    std::uint32_t low = 0, high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    if ((low & 0xE6U) != 0xE6U ||
        !__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return false;
    constexpr unsigned int instruction_bits = (1U << 5) | (1U << 16);
    return (ebx & instruction_bits) == instruction_bits;  // AVX2 + AVX512F.
  }();
  return supported;
}

// Same coordinate arithmetic, clipping and four-axis layout as encode_avx2.
// AVX512F handles doubles; AVX2/SSE pack the eight int32 codes without requiring
// AVX512BW/DQ/VL. Only the positive residual-sum association changes.
template <typename Rows>
__attribute__((target("avx2,avx512f"))) inline double encode_avx512(
    const Rows &rows, std::size_t dimension, double scale, std::size_t stride,
    bool blocked, std::int8_t *codes, std::uint32_t *norms,
    double center = 0) noexcept {
  const __m512d multiplier = _mm512_set1_pd(scale);
  const __m512d origin = _mm512_set1_pd(center);
  const __m512i sign = _mm512_castpd_si512(_mm512_set1_pd(-0.0));
  const __m512i half = _mm512_castpd_si512(_mm512_set1_pd(0.5));
  const __m256i lower = _mm256_set1_epi32(-127);
  const __m256i upper = _mm256_set1_epi32(127);
  const __m128i offset = _mm_set1_epi8(-128);
  double maximum_error = 0;
  std::size_t index = 0;
  for (const auto &row : rows) {
    __m512d error_sums = _mm512_setzero_pd();
    __m256i norm_sums = _mm256_setzero_si256();
    const std::size_t base = blocked
        ? index / 16 * 16 * stride + index % 16 * 4 : index * stride;
    std::size_t axis = 0;
    for (; axis + 8 <= dimension; axis += 8) {
      const __m512d scaled = _mm512_mul_pd(
          _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(row.vector_data + axis)),
                        origin), multiplier);
      const __m512d adjustment = _mm512_castsi512_pd(_mm512_or_si512(
          half, _mm512_and_si512(sign, _mm512_castpd_si512(scaled))));
      const __m256i code = _mm256_min_epi32(upper, _mm256_max_epi32(lower,
          _mm512_cvttpd_epi32(_mm512_add_pd(scaled, adjustment))));
      norm_sums = _mm256_add_epi32(norm_sums, _mm256_mullo_epi32(code, code));
      const __m512d error = _mm512_sub_pd(scaled, _mm512_cvtepi32_pd(code));
      error_sums = _mm512_add_pd(error_sums, _mm512_mul_pd(error, error));
      const __m128i words = _mm_packs_epi32(_mm256_castsi256_si128(code),
                                          _mm256_extracti128_si256(code, 1));
      __m128i bytes = _mm_packs_epi16(words, words);
      if (blocked) bytes = _mm_xor_si128(bytes, offset);
      const std::uint32_t packed[2] = {
          static_cast<std::uint32_t>(_mm_cvtsi128_si32(bytes)),
          static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_srli_si128(bytes, 4)))};
      if (blocked) {
        std::memcpy(codes + base + axis * 16, &packed[0], sizeof(packed[0]));
        std::memcpy(codes + base + (axis + 4) * 16, &packed[1], sizeof(packed[1]));
      } else {
        std::memcpy(codes + base + axis, packed, sizeof(packed));
      }
    }
    alignas(64) double error_lanes[8];
    alignas(32) std::uint32_t norm_lanes[8];
    _mm512_store_pd(error_lanes, error_sums);
    _mm256_store_si256(reinterpret_cast<__m256i *>(norm_lanes), norm_sums);
    double error_sum = ((error_lanes[0] + error_lanes[1]) +
                        (error_lanes[2] + error_lanes[3])) +
                       ((error_lanes[4] + error_lanes[5]) +
                        (error_lanes[6] + error_lanes[7]));
    std::uint32_t norm = norm_lanes[0] + norm_lanes[1] +
                         norm_lanes[2] + norm_lanes[3] +
                         norm_lanes[4] + norm_lanes[5] +
                         norm_lanes[6] + norm_lanes[7];
    for (; axis < dimension; ++axis) {
      const double scaled =
          (static_cast<double>(row.vector_data[axis]) - center) * scale;
      const int code = std::clamp(static_cast<int>(
          scaled + std::copysign(0.5, scaled)), -127, 127);
      const std::size_t position = blocked
          ? base + axis / 4 * 64 + axis % 4 : base + axis;
      const auto byte = static_cast<std::uint8_t>(code + (blocked ? 128 : 0));
      std::memcpy(codes + position, &byte, sizeof(byte));
      norm += static_cast<std::uint32_t>(code * code);
      const double error = scaled - code;
      error_sum += error * error;
    }
    norms[index++] = norm;
    maximum_error = std::max(maximum_error, error_sum);
  }
  return maximum_error;
}

template <typename Rows>
inline double encode(
    const Rows &rows, std::size_t dimension, double scale, std::size_t stride,
    bool blocked, std::int8_t *codes, std::uint32_t *norms,
    double center = 0) noexcept {
  if (avx512_available()) {
    return encode_avx512(rows, dimension, scale, stride, blocked, codes, norms, center);
  }
  return encode_avx2(rows, dimension, scale, stride, blocked, codes, norms, center);
}

#endif

}  // namespace mvd::quantized_prepare

#endif
