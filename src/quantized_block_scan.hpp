#ifndef MVD_QUANTIZED_BLOCK_SCAN_HPP
#define MVD_QUANTIZED_BLOCK_SCAN_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && \
    (defined(__x86_64__) || defined(__i386__))
#define MVD_QUANTIZED_BLOCK_X86_TARGETS 1
#include <cpuid.h>
#include <immintrin.h>
#else
#define MVD_QUANTIZED_BLOCK_X86_TARGETS 0
#endif

namespace mvd::quantized_block {

inline constexpr std::size_t kBlockRows = 16;
inline constexpr std::size_t kDimensionGroup = 4;
inline constexpr std::size_t kMinDimension = 16;
inline constexpr std::size_t kMaxDimension = 16384;

constexpr bool valid_padded_dimension(std::size_t dimension) noexcept {
  return dimension >= kMinDimension && dimension <= kMaxDimension &&
         dimension % kDimensionGroup == 0;
}

constexpr std::size_t padded_dimension(std::size_t dimension) noexcept {
  assert(dimension >= kMinDimension && dimension <= kMaxDimension);
  return (dimension + kDimensionGroup - 1) / kDimensionGroup * kDimensionGroup;
}

constexpr std::size_t packed_bytes(std::size_t padded_dimension) noexcept {
  assert(valid_padded_dimension(padded_dimension));
  return kBlockRows * padded_dimension;
}

// Codes are in [-127, 127]; queries may use the full int8 range [-128, 127].
// Compute this once per query over its actual dimension. Zero query padding
// gives the same correction if the caller instead passes the padded dimension.
inline std::int64_t query_correction(const std::int8_t *query,
                                     std::size_t dimension) noexcept {
  assert(query != nullptr);
  assert(dimension >= kMinDimension && dimension <= kMaxDimension);
  std::int64_t correction = 0;
  for (std::size_t axis = 0; axis < dimension; ++axis) {
    const std::int64_t q = query[axis];
    correction += q * q + 256 * q;
  }
  return correction;
}

// One block contains 16 rows. Each group of four axes occupies 64 bytes:
//   packed[(axis / 4) * 64 + row * 4 + axis % 4] = code[row][axis] + 128.
// Missing axes and missing rows contain 128; missing query axes contain zero.
// row_norms holds 16 native uint32 values, computed over the actual dimension.
// Missing rows have norm zero. All 16 scores are written, including the zero-
// vector distances for missing rows; the caller must ignore those extra rows.
//
// Preconditions: padded_dimension is a multiple of four in [16, 16384]; packed
// and query expose 16 * padded_dimension and padded_dimension readable bytes;
// row_norms and scores expose 64 readable/writable bytes respectively. Scores
// must not overlap the input buffers. No alignment is required, even for norms
// and scores: their void pointers deliberately permit byte-unaligned storage.
//
// With valid codes, norms and correction, |dot| < 2^29, all score arithmetic
// fits int32, and each squared distance is at most 255^2 * 16384 < 2^30.
using Kernel = void (*)(const std::uint8_t *packed, const std::int8_t *query,
                       std::size_t padded_dimension, const void *row_norms,
                       std::int64_t correction, void *scores) noexcept;

// Return one bit for each code distance <= above. The 16-row API consumes
// one packed block; the 64-row API consumes four consecutive packed blocks.
// A stale (larger) boundary may admit extra rows, but cannot drop a survivor.
using MaskKernel = std::uint64_t (*)(const std::uint8_t *packed,
                                    const std::int8_t *query,
                                    std::size_t padded_dimension,
                                    const void *row_norms,
                                    std::int64_t correction,
                                    std::uint32_t above) noexcept;

inline void scalar(const std::uint8_t *packed, const std::int8_t *query,
                   std::size_t padded_dimension, const void *row_norms,
                   std::int64_t correction, void *scores) noexcept {
  assert(valid_padded_dimension(padded_dimension));
  assert(packed != nullptr && query != nullptr && row_norms != nullptr &&
         scores != nullptr);
  const auto *norm_bytes = static_cast<const std::uint8_t *>(row_norms);
  auto *score_bytes = static_cast<std::uint8_t *>(scores);
  for (std::size_t row = 0; row < kBlockRows; ++row) {
    std::int32_t dot = 0;
    for (std::size_t axis = 0; axis < padded_dimension; axis += 4) {
      const auto *codes = packed + axis * kBlockRows + row * 4;
      for (std::size_t lane = 0; lane < 4; ++lane) {
        dot += static_cast<std::int32_t>(codes[lane]) * query[axis + lane];
      }
    }
    std::uint32_t norm = 0;
    std::memcpy(&norm, norm_bytes + row * sizeof(norm), sizeof(norm));
    const auto score = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(norm) + correction - 2LL * dot);
    std::memcpy(score_bytes + row * sizeof(score), &score, sizeof(score));
  }
}

inline std::uint64_t mask_scalar(const std::uint8_t *packed,
                                 const std::int8_t *query,
                                 std::size_t padded_dimension,
                                 const void *row_norms,
                                 std::int64_t correction,
                                 std::uint32_t above) noexcept {
  std::uint32_t scores[kBlockRows];
  scalar(packed, query, padded_dimension, row_norms, correction, scores);
  std::uint64_t mask = 0;
  for (std::size_t row = 0; row < kBlockRows; ++row) {
    mask |= std::uint64_t{scores[row] <= above} << row;
  }
  return mask;
}

inline std::uint64_t mask64_scalar(const std::uint8_t *packed,
                                   const std::int8_t *query,
                                   std::size_t padded_dimension,
                                   const void *row_norms,
                                   std::int64_t correction,
                                   std::uint32_t above) noexcept {
  std::uint64_t mask = 0;
  const auto *norm_bytes = static_cast<const std::uint8_t *>(row_norms);
  for (std::size_t block = 0; block < 4; ++block) {
    mask |= mask_scalar(packed + block * packed_bytes(padded_dimension),
                        query, padded_dimension,
                        norm_bytes + block * kBlockRows * sizeof(std::uint32_t),
                        correction, above) << (block * kBlockRows);
  }
  return mask;
}

enum class KernelKind { scalar, avx2, avx512bw, vnni512 };

struct CpuFeatures {
  bool avx2 = false;
  bool avx512bw = false;
  bool vnni512 = false;
};

namespace detail {

inline CpuFeatures detect_cpu_features() noexcept {
  CpuFeatures features;
#if MVD_QUANTIZED_BLOCK_X86_TARGETS
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
    return features;
  }
  // XGETBV itself is gated by XSAVE, OSXSAVE and AVX. Both CPU support and
  // OS-managed register state are required before returning any SIMD kernel.
  constexpr unsigned int required = (1U << 26) | (1U << 27) | (1U << 28);
  if ((ecx & required) != required) {
    return features;
  }
  std::uint32_t xcr0_low = 0, xcr0_high = 0;
  __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
  const std::uint64_t xcr0 =
      (static_cast<std::uint64_t>(xcr0_high) << 32) | xcr0_low;
  if ((xcr0 & 0x6U) != 0x6U ||
      !__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
    return features;
  }
  features.avx2 = (ebx & (1U << 5)) != 0;
  constexpr unsigned int avx512_bits = (1U << 16) | (1U << 30);
  features.avx512bw = features.avx2 && (xcr0 & 0xE6U) == 0xE6U &&
                      (ebx & avx512_bits) == avx512_bits;
  features.vnni512 = features.avx512bw && (ecx & (1U << 11)) != 0;
#endif
  return features;
}

#if MVD_QUANTIZED_BLOCK_X86_TARGETS

// Internal ISA-specific entry points. Obtain a callable pointer through
// kernel_for()/best_kernel(), so an unsupported instruction is never entered.
__attribute__((target("avx2"))) inline void scan_avx2(
    const std::uint8_t *packed, const std::int8_t *query,
    std::size_t padded_dimension, const void *row_norms,
    std::int64_t correction, void *scores) noexcept {
  assert(valid_padded_dimension(padded_dimension));
  assert(packed != nullptr && query != nullptr && row_norms != nullptr &&
         scores != nullptr);
  __m256i dot0 = _mm256_setzero_si256();
  __m256i dot1 = _mm256_setzero_si256();
  const __m256i low_mask = _mm256_set1_epi8(0x7f);
  const __m256i ones = _mm256_set1_epi16(1);
  for (std::size_t axis = 0; axis < padded_dimension; axis += 4) {
    std::int32_t query_word = 0;
    std::memcpy(&query_word, query + axis, sizeof(query_word));
    const __m256i q = _mm256_set1_epi32(query_word);
    const auto *group = packed + axis * kBlockRows;
    const __m256i rows0 =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(group));
    const __m256i rows1 =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(group + 32));

    // Split u into (u & 127) + (u & 128). Even with q == -128, each
    // maddubs pair lies in [-32512, 32258] or [-32768, 32512]. Widen each
    // partial pair to int32 BEFORE adding the two parts: no saturation occurs.
    const __m256i low0 =
        _mm256_maddubs_epi16(_mm256_and_si256(rows0, low_mask), q);
    const __m256i high0 =
        _mm256_maddubs_epi16(_mm256_andnot_si256(low_mask, rows0), q);
    const __m256i low1 =
        _mm256_maddubs_epi16(_mm256_and_si256(rows1, low_mask), q);
    const __m256i high1 =
        _mm256_maddubs_epi16(_mm256_andnot_si256(low_mask, rows1), q);
    dot0 = _mm256_add_epi32(
        dot0, _mm256_add_epi32(_mm256_madd_epi16(low0, ones),
                               _mm256_madd_epi16(high0, ones)));
    dot1 = _mm256_add_epi32(
        dot1, _mm256_add_epi32(_mm256_madd_epi16(low1, ones),
                               _mm256_madd_epi16(high1, ones)));
  }
  const auto *norm_bytes = static_cast<const std::uint8_t *>(row_norms);
  auto *score_bytes = static_cast<std::uint8_t *>(scores);
  const __m256i correction32 =
      _mm256_set1_epi32(static_cast<std::int32_t>(correction));
  const __m256i base0 = _mm256_add_epi32(
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(norm_bytes)),
      correction32);
  const __m256i base1 = _mm256_add_epi32(
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(norm_bytes + 32)),
      correction32);
  _mm256_storeu_si256(reinterpret_cast<__m256i *>(score_bytes),
                     _mm256_sub_epi32(base0, _mm256_add_epi32(dot0, dot0)));
  _mm256_storeu_si256(reinterpret_cast<__m256i *>(score_bytes + 32),
                     _mm256_sub_epi32(base1, _mm256_add_epi32(dot1, dot1)));
}

__attribute__((target("avx512f,avx512bw"))) inline void scan_avx512bw(
    const std::uint8_t *packed, const std::int8_t *query,
    std::size_t padded_dimension, const void *row_norms,
    std::int64_t correction, void *scores) noexcept {
  assert(valid_padded_dimension(padded_dimension));
  assert(packed != nullptr && query != nullptr && row_norms != nullptr &&
         scores != nullptr);
  __m512i dot = _mm512_setzero_si512();
  const __m512i low_mask = _mm512_set1_epi8(0x7f);
  const __m512i ones = _mm512_set1_epi16(1);
  for (std::size_t axis = 0; axis < padded_dimension; axis += 4) {
    std::int32_t query_word = 0;
    std::memcpy(&query_word, query + axis, sizeof(query_word));
    const __m512i q = _mm512_set1_epi32(query_word);
    const __m512i rows = _mm512_loadu_si512(packed + axis * kBlockRows);
    const __m512i low =
        _mm512_maddubs_epi16(_mm512_and_si512(rows, low_mask), q);
    const __m512i high =
        _mm512_maddubs_epi16(_mm512_andnot_si512(low_mask, rows), q);
    // As in AVX2, widen the two safe signed-16 partial sums separately.
    dot = _mm512_add_epi32(
        dot, _mm512_add_epi32(_mm512_madd_epi16(low, ones),
                              _mm512_madd_epi16(high, ones)));
  }
  const __m512i base = _mm512_add_epi32(
      _mm512_loadu_si512(row_norms),
      _mm512_set1_epi32(static_cast<std::int32_t>(correction)));
  _mm512_storeu_si512(scores,
                     _mm512_sub_epi32(base, _mm512_add_epi32(dot, dot)));
}

__attribute__((target("avx512f,avx512bw,avx512vnni"))) inline void scan_vnni512(
    const std::uint8_t *packed, const std::int8_t *query,
    std::size_t padded_dimension, const void *row_norms,
    std::int64_t correction, void *scores) noexcept {
  assert(valid_padded_dimension(padded_dimension));
  assert(packed != nullptr && query != nullptr && row_norms != nullptr &&
         scores != nullptr);
  __m512i dot0 = _mm512_setzero_si512();
  __m512i dot1 = _mm512_setzero_si512();
  __m512i dot2 = _mm512_setzero_si512();
  __m512i dot3 = _mm512_setzero_si512();
  std::size_t axis = 0;
  // Four independent dependency chains, each with sixteen row accumulators.
  // dpbusd is non-saturating; the final additions preserve the row lanes.
  for (; axis + 16 <= padded_dimension; axis += 16) {
    std::int32_t query_word0 = 0, query_word1 = 0;
    std::int32_t query_word2 = 0, query_word3 = 0;
    std::memcpy(&query_word0, query + axis, sizeof(query_word0));
    std::memcpy(&query_word1, query + axis + 4, sizeof(query_word1));
    std::memcpy(&query_word2, query + axis + 8, sizeof(query_word2));
    std::memcpy(&query_word3, query + axis + 12, sizeof(query_word3));
    const auto *group = packed + axis * kBlockRows;
    dot0 = _mm512_dpbusd_epi32(dot0, _mm512_loadu_si512(group),
                               _mm512_set1_epi32(query_word0));
    dot1 = _mm512_dpbusd_epi32(dot1, _mm512_loadu_si512(group + 64),
                               _mm512_set1_epi32(query_word1));
    dot2 = _mm512_dpbusd_epi32(dot2, _mm512_loadu_si512(group + 128),
                               _mm512_set1_epi32(query_word2));
    dot3 = _mm512_dpbusd_epi32(dot3, _mm512_loadu_si512(group + 192),
                               _mm512_set1_epi32(query_word3));
  }
  // A padded dimension can leave up to three complete four-axis groups.
  for (; axis < padded_dimension; axis += 4) {
    std::int32_t query_word = 0;
    std::memcpy(&query_word, query + axis, sizeof(query_word));
    const __m512i q = _mm512_set1_epi32(query_word);
    const __m512i rows = _mm512_loadu_si512(packed + axis * kBlockRows);
    dot0 = _mm512_dpbusd_epi32(dot0, rows, q);
  }
  const __m512i dot = _mm512_add_epi32(_mm512_add_epi32(dot0, dot1),
                                      _mm512_add_epi32(dot2, dot3));
  const __m512i base = _mm512_add_epi32(
      _mm512_loadu_si512(row_norms),
      _mm512_set1_epi32(static_cast<std::int32_t>(correction)));
  _mm512_storeu_si512(scores,
                     _mm512_sub_epi32(base, _mm512_add_epi32(dot, dot)));
}

__attribute__((target("avx2"))) inline std::uint64_t mask_avx2(
    const std::uint8_t *packed, const std::int8_t *query,
    std::size_t padded_dimension, const void *row_norms,
    std::int64_t correction, std::uint32_t above) noexcept {
  alignas(32) std::uint32_t scores[kBlockRows];
  scan_avx2(packed, query, padded_dimension, row_norms, correction, scores);
  // Biasing both operands gives an unsigned comparison even for a boundary
  // of UINT32_MAX. Code distances themselves fit below 2^30.
  const __m256i bias = _mm256_set1_epi32(-2147483647 - 1);
  const __m256i boundary = _mm256_xor_si256(
      _mm256_set1_epi32(static_cast<std::int32_t>(above)), bias);
  const __m256i value0 = _mm256_xor_si256(
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(scores)), bias);
  const __m256i value1 = _mm256_xor_si256(
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(scores + 8)), bias);
  const unsigned rejected0 = static_cast<unsigned>(_mm256_movemask_ps(
      _mm256_castsi256_ps(_mm256_cmpgt_epi32(value0, boundary))));
  const unsigned rejected1 = static_cast<unsigned>(_mm256_movemask_ps(
      _mm256_castsi256_ps(_mm256_cmpgt_epi32(value1, boundary))));
  return static_cast<std::uint64_t>(
      (~(rejected0 | (rejected1 << 8))) & 0xffffU);
}

__attribute__((target("avx512f,avx512bw"))) inline std::uint64_t mask_avx512bw(
    const std::uint8_t *packed, const std::int8_t *query,
    std::size_t padded_dimension, const void *row_norms,
    std::int64_t correction, std::uint32_t above) noexcept {
  alignas(64) std::uint32_t scores[kBlockRows];
  scan_avx512bw(packed, query, padded_dimension, row_norms, correction, scores);
  return _mm512_cmple_epu32_mask(_mm512_loadu_si512(scores),
      _mm512_set1_epi32(static_cast<std::int32_t>(above)));
}

__attribute__((target("avx512f,avx512bw,avx512vnni")))
inline std::uint64_t mask_vnni512(
    const std::uint8_t *packed, const std::int8_t *query,
    std::size_t padded_dimension, const void *row_norms,
    std::int64_t correction, std::uint32_t above) noexcept {
  alignas(64) std::uint32_t scores[kBlockRows];
  scan_vnni512(packed, query, padded_dimension, row_norms, correction, scores);
  return _mm512_cmple_epu32_mask(_mm512_loadu_si512(scores),
      _mm512_set1_epi32(static_cast<std::int32_t>(above)));
}

__attribute__((target("avx512f,avx512bw,avx512vnni")))
inline std::uint64_t mask64_vnni512(
    const std::uint8_t *packed, const std::int8_t *query,
    std::size_t padded_dimension, const void *row_norms,
    std::int64_t correction, std::uint32_t above) noexcept {
  assert(valid_padded_dimension(padded_dimension));
  assert(packed != nullptr && query != nullptr && row_norms != nullptr);
  const std::size_t stride = packed_bytes(padded_dimension);
  __m512i dot0 = _mm512_setzero_si512();
  __m512i dot1 = _mm512_setzero_si512();
  __m512i dot2 = _mm512_setzero_si512();
  __m512i dot3 = _mm512_setzero_si512();
  // Reuse each query broadcast for four blocks. These four independent
  // accumulators hide dpbusd latency without horizontal row reductions.
  for (std::size_t axis = 0; axis < padded_dimension; axis += 4) {
    std::int32_t word = 0;
    std::memcpy(&word, query + axis, sizeof(word));
    const __m512i q = _mm512_set1_epi32(word);
    const auto *group = packed + axis * kBlockRows;
    dot0 = _mm512_dpbusd_epi32(dot0, _mm512_loadu_si512(group), q);
    dot1 = _mm512_dpbusd_epi32(dot1, _mm512_loadu_si512(group + stride), q);
    dot2 = _mm512_dpbusd_epi32(dot2, _mm512_loadu_si512(group + 2 * stride), q);
    dot3 = _mm512_dpbusd_epi32(dot3, _mm512_loadu_si512(group + 3 * stride), q);
  }
  const auto *norm_bytes = static_cast<const std::uint8_t *>(row_norms);
  const __m512i adjustment =
      _mm512_set1_epi32(static_cast<std::int32_t>(correction));
  const __m512i boundary = _mm512_set1_epi32(static_cast<std::int32_t>(above));
  const __m512i base0 = _mm512_add_epi32(_mm512_loadu_si512(norm_bytes), adjustment);
  const __m512i base1 = _mm512_add_epi32(_mm512_loadu_si512(norm_bytes + 64), adjustment);
  const __m512i base2 = _mm512_add_epi32(_mm512_loadu_si512(norm_bytes + 128), adjustment);
  const __m512i base3 = _mm512_add_epi32(_mm512_loadu_si512(norm_bytes + 192), adjustment);
  const std::uint64_t mask0 = _mm512_cmple_epu32_mask(
      _mm512_sub_epi32(base0, _mm512_add_epi32(dot0, dot0)), boundary);
  const std::uint64_t mask1 = _mm512_cmple_epu32_mask(
      _mm512_sub_epi32(base1, _mm512_add_epi32(dot1, dot1)), boundary);
  const std::uint64_t mask2 = _mm512_cmple_epu32_mask(
      _mm512_sub_epi32(base2, _mm512_add_epi32(dot2, dot2)), boundary);
  const std::uint64_t mask3 = _mm512_cmple_epu32_mask(
      _mm512_sub_epi32(base3, _mm512_add_epi32(dot3, dot3)), boundary);
  return mask0 | (mask1 << 16) | (mask2 << 32) | (mask3 << 48);
}

#endif

}  // namespace detail

// Lazy initialization: neither CPU detection nor vector setup runs in a
// namespace-scope initializer. Non-GNU/Clang-x86 builds retain the scalar API.
inline CpuFeatures cpu_features() noexcept {
  static const CpuFeatures features = detail::detect_cpu_features();
  return features;
}

// Resolve once outside a scan loop. Unsupported SIMD choices return nullptr;
// the scalar choice is always available. Do not call detail kernels directly.
inline Kernel kernel_for(KernelKind kind) noexcept {
  if (kind == KernelKind::scalar) {
    return &scalar;
  }
#if MVD_QUANTIZED_BLOCK_X86_TARGETS
  const CpuFeatures features = cpu_features();
  switch (kind) {
    case KernelKind::avx2:
      return features.avx2 ? &detail::scan_avx2 : nullptr;
    case KernelKind::avx512bw:
      return features.avx512bw ? &detail::scan_avx512bw : nullptr;
    case KernelKind::vnni512:
      return features.vnni512 ? &detail::scan_vnni512 : nullptr;
    default:
      break;
  }
#endif
  return nullptr;
}

inline Kernel best_kernel() noexcept {
#if MVD_QUANTIZED_BLOCK_X86_TARGETS
  const CpuFeatures features = cpu_features();
  if (features.vnni512) return &detail::scan_vnni512;
  if (features.avx512bw) return &detail::scan_avx512bw;
  if (features.avx2) return &detail::scan_avx2;
#endif
  return &scalar;
}

inline MaskKernel mask_kernel_for(KernelKind kind) noexcept {
  if (kind == KernelKind::scalar) return &mask_scalar;
#if MVD_QUANTIZED_BLOCK_X86_TARGETS
  const CpuFeatures features = cpu_features();
  switch (kind) {
    case KernelKind::avx2:
      return features.avx2 ? &detail::mask_avx2 : nullptr;
    case KernelKind::avx512bw:
      return features.avx512bw ? &detail::mask_avx512bw : nullptr;
    case KernelKind::vnni512:
      return features.vnni512 ? &detail::mask_vnni512 : nullptr;
    default:
      break;
  }
#endif
  return nullptr;
}

inline MaskKernel mask64_kernel_for(KernelKind kind) noexcept {
  if (kind == KernelKind::scalar) return &mask64_scalar;
#if MVD_QUANTIZED_BLOCK_X86_TARGETS
  if (kind == KernelKind::vnni512 && cpu_features().vnni512) {
    return &detail::mask64_vnni512;
  }
#endif
  return nullptr;
}

}  // namespace mvd::quantized_block

#undef MVD_QUANTIZED_BLOCK_X86_TARGETS
#endif  // MVD_QUANTIZED_BLOCK_SCAN_HPP
