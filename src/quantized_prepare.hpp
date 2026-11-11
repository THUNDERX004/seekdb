#ifndef MVD_QUANTIZED_PREPARE_HPP
#define MVD_QUANTIZED_PREPARE_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if !defined(MVD_FORCE_SCALAR) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
#define MVD_QUANTIZED_PREPARE_AVX2 1
#include <immintrin.h>
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

// Inputs are finite binary32 values. Absolute value and maximum introduce no
// rounding, and the result is widened to double only after the reduction.
template <typename Rows>
__attribute__((target("avx2"))) inline double maximum_absolute(
    const Rows &rows, std::size_t dimension) noexcept {
  __m256 maximum = _mm256_setzero_ps();
  const __m256 sign = _mm256_set1_ps(-0.0f);
  float tail_maximum = 0;
  for (const auto &row : rows) {
    std::size_t axis = 0;
    for (; axis + 8 <= dimension; axis += 8) {
      const __m256 value = _mm256_loadu_ps(row.vector_data + axis);
      maximum = _mm256_max_ps(maximum, _mm256_andnot_ps(sign, value));
    }
    for (; axis < dimension; ++axis) {
      tail_maximum = std::max(tail_maximum, std::abs(row.vector_data[axis]));
    }
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, maximum);
  for (float value : lanes) tail_maximum = std::max(tail_maximum, value);
  return tail_maximum;
}

// Scale is a power of two, so widening and multiplication remain exact even
// for subnormal float32 coordinates. The strict |scaled| < 128 contract is
// established by the caller's maximum/exponent calculation. Rounding ties
// away from zero matches std::round; clamping preserves the signed code range.
// Only the order of positive residual-square additions changes. The caller
// applies its conservative double-precision error envelope to this maximum.
template <typename Rows>
__attribute__((target("avx2"))) inline double encode(
    const Rows &rows, std::size_t dimension, double scale, std::size_t stride,
    bool blocked, std::int8_t *codes, std::uint32_t *norms) noexcept {
  const __m256d multiplier = _mm256_set1_pd(scale);
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
          _mm256_cvtps_pd(_mm_loadu_ps(row.vector_data + axis)), multiplier);
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
      const double scaled = static_cast<double>(row.vector_data[axis]) * scale;
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

#endif

}  // namespace mvd::quantized_prepare

#endif
