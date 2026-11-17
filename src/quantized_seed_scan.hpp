#ifndef MVD_QUANTIZED_SEED_SCAN_HPP
#define MVD_QUANTIZED_SEED_SCAN_HPP
#include "quantized_block_scan.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>

namespace mvd::quantized_seed {
using ScanKernel = void (*)(const std::uint8_t *,const std::int8_t *,std::size_t,
    const std::uint32_t *,std::int64_t,std::size_t,std::uint32_t *,
    std::uint32_t *,std::uint32_t *) noexcept;
using MaskKernel = std::uint64_t (*)(const std::uint32_t *,std::uint32_t) noexcept;

// One synchronous range owns each workspace. The kernels overwrite every
// readable score, including padded tail lanes, before screening reads it.
class Scratch {
 public:
  bool prepare(std::size_t rows) noexcept {
    constexpr std::size_t max_rows=65600; // 65536 real rows plus block padding.
    if (rows>max_rows) return false;
    if (rows<=capacity_) return true;
    try {
      std::unique_ptr<std::uint32_t[]> staged(new std::uint32_t[rows]);
      scores_.swap(staged);
      capacity_=rows;
      return true;
    } catch (const std::bad_alloc &) { return false; }
  }
  std::uint32_t *data() noexcept { return scores_.get(); }

 private:
  std::unique_ptr<std::uint32_t[]> scores_;
  std::size_t capacity_=0;
};

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && \
    (defined(__x86_64__) || defined(__i386__))
namespace detail {
template <std::size_t Dimension>
__attribute__((target("avx512f,avx512bw,avx512vnni")))
inline void scan(const std::uint8_t *packed,const std::int8_t *query,
    std::size_t dimension,const std::uint32_t *norms,std::int64_t correction,
    std::size_t rows,std::uint32_t *scores,std::uint32_t *seed_scores,
    std::uint32_t *seed_rows) noexcept {
  static_assert(Dimension==64 || Dimension==128 || Dimension==256);
  assert(dimension==Dimension && rows>=64 && rows%64==0);
  (void)dimension;
  const __m512i adjustment=_mm512_set1_epi32(static_cast<std::int32_t>(correction));
  const __m512i lanes=_mm512_set_epi32(15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0);
  __m512i best0=_mm512_set1_epi32(-1);
  __m512i row0=_mm512_setzero_si512();
  __m512i best1=_mm512_set1_epi32(-1);
  __m512i row1=_mm512_setzero_si512();
  __m512i best2=_mm512_set1_epi32(-1);
  __m512i row2=_mm512_setzero_si512();
  __m512i best3=_mm512_set1_epi32(-1);
  __m512i row3=_mm512_setzero_si512();
  for (std::size_t first=0;first<rows;first+=64) {
    __m512i dot0=_mm512_setzero_si512();
    __m512i dot1=_mm512_setzero_si512();
    __m512i dot2=_mm512_setzero_si512();
    __m512i dot3=_mm512_setzero_si512();
#pragma GCC unroll 32
    for (std::size_t axis=0;axis<Dimension;axis+=4) {
      std::int32_t word=0;
      std::memcpy(&word,query+axis,sizeof(word));
      const __m512i q=_mm512_set1_epi32(word);
      const auto *group=packed+first*Dimension+axis*16;
      dot0=_mm512_dpbusd_epi32(dot0,_mm512_loadu_si512(group+0*16*Dimension),q);
      dot1=_mm512_dpbusd_epi32(dot1,_mm512_loadu_si512(group+1*16*Dimension),q);
      dot2=_mm512_dpbusd_epi32(dot2,_mm512_loadu_si512(group+2*16*Dimension),q);
      dot3=_mm512_dpbusd_epi32(dot3,_mm512_loadu_si512(group+3*16*Dimension),q);
    }
    const __m512i score0=_mm512_sub_epi32(
        _mm512_add_epi32(_mm512_loadu_si512(norms+first+0),adjustment),
        _mm512_add_epi32(dot0,dot0));
    _mm512_storeu_si512(scores+first+0,score0);
    const __mmask16 better0=_mm512_cmplt_epu32_mask(score0,best0);
    best0=_mm512_min_epu32(best0,score0);
    row0=_mm512_mask_mov_epi32(row0,better0,_mm512_add_epi32(lanes,
        _mm512_set1_epi32(static_cast<std::int32_t>(first+0))));
    const __m512i score1=_mm512_sub_epi32(
        _mm512_add_epi32(_mm512_loadu_si512(norms+first+16),adjustment),
        _mm512_add_epi32(dot1,dot1));
    _mm512_storeu_si512(scores+first+16,score1);
    const __mmask16 better1=_mm512_cmplt_epu32_mask(score1,best1);
    best1=_mm512_min_epu32(best1,score1);
    row1=_mm512_mask_mov_epi32(row1,better1,_mm512_add_epi32(lanes,
        _mm512_set1_epi32(static_cast<std::int32_t>(first+16))));
    const __m512i score2=_mm512_sub_epi32(
        _mm512_add_epi32(_mm512_loadu_si512(norms+first+32),adjustment),
        _mm512_add_epi32(dot2,dot2));
    _mm512_storeu_si512(scores+first+32,score2);
    const __mmask16 better2=_mm512_cmplt_epu32_mask(score2,best2);
    best2=_mm512_min_epu32(best2,score2);
    row2=_mm512_mask_mov_epi32(row2,better2,_mm512_add_epi32(lanes,
        _mm512_set1_epi32(static_cast<std::int32_t>(first+32))));
    const __m512i score3=_mm512_sub_epi32(
        _mm512_add_epi32(_mm512_loadu_si512(norms+first+48),adjustment),
        _mm512_add_epi32(dot3,dot3));
    _mm512_storeu_si512(scores+first+48,score3);
    const __mmask16 better3=_mm512_cmplt_epu32_mask(score3,best3);
    best3=_mm512_min_epu32(best3,score3);
    row3=_mm512_mask_mov_epi32(row3,better3,_mm512_add_epi32(lanes,
        _mm512_set1_epi32(static_cast<std::int32_t>(first+48))));
  }
  _mm512_storeu_si512(seed_scores+0,best0);
  _mm512_storeu_si512(seed_rows+0,row0);
  _mm512_storeu_si512(seed_scores+16,best1);
  _mm512_storeu_si512(seed_rows+16,row1);
  _mm512_storeu_si512(seed_scores+32,best2);
  _mm512_storeu_si512(seed_rows+32,row2);
  _mm512_storeu_si512(seed_scores+48,best3);
  _mm512_storeu_si512(seed_rows+48,row3);
}

__attribute__((target("avx512f"))) inline std::uint64_t mask64(
    const std::uint32_t *scores,std::uint32_t above) noexcept {
  const __m512i boundary=_mm512_set1_epi32(static_cast<std::int32_t>(above));
  return static_cast<std::uint64_t>(_mm512_cmple_epu32_mask(_mm512_loadu_si512(scores),boundary)) |
      (static_cast<std::uint64_t>(_mm512_cmple_epu32_mask(_mm512_loadu_si512(scores+16),boundary)) << 16) |
      (static_cast<std::uint64_t>(_mm512_cmple_epu32_mask(_mm512_loadu_si512(scores+32),boundary)) << 32) |
      (static_cast<std::uint64_t>(_mm512_cmple_epu32_mask(_mm512_loadu_si512(scores+48),boundary)) << 48);
}
}  // namespace detail
#endif

inline ScanKernel scan_kernel(std::size_t dimension) noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && \
    (defined(__x86_64__) || defined(__i386__))
  if (quantized_block::cpu_features().vnni512) {
    if (dimension==64) return &detail::scan<64>;
    if (dimension==128) return &detail::scan<128>;
    if (dimension==256) return &detail::scan<256>;
  }
#else
  (void)dimension;
#endif
  return nullptr;
}

inline MaskKernel mask_kernel() noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && \
    (defined(__x86_64__) || defined(__i386__))
  if (quantized_block::cpu_features().vnni512) return &detail::mask64;
#endif
  return nullptr;
}

// Packed rows start at a 16-row block boundary, leading_rows lanes before the
// requested range. Packed bytes and norms expose ceil((leading_rows+rows)/16)*16
// padded rows; only the requested rows reach callbacks. Its query has dimension
// readable bytes including zero padding.
// Only scan order changes: seed rows enter the ordinary exact heap, and all
// other rows are screened against its outward-rounded code-distance bound.
// Keep scratch and seed selection out of small-list scan frames.
template <typename ConsumeSeed, typename ConsumeSurvivor, typename Boundary>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool visit(
    const std::uint8_t *packed,const std::int8_t *query,std::size_t dimension,
    const std::uint32_t *norms,std::int64_t correction,std::size_t rows,
    std::size_t first_row,std::size_t capacity,const ConsumeSeed &consume_seed,
    const ConsumeSurvivor &consume_survivor,const Boundary &boundary,
    std::size_t leading_rows=0,Scratch *workspace=nullptr) {
  constexpr std::size_t max_scratch_rows=65536;
  if (rows<512 || rows>max_scratch_rows || capacity==0 || capacity>32 ||
      leading_rows>=quantized_block::kBlockRows) return false;
  const auto scan=scan_kernel(dimension);
  if (scan==nullptr) return false;
  const std::size_t extent=leading_rows+rows;
  Scratch local;
  Scratch &scores=workspace==nullptr?local:*workspace;
  if (!scores.prepare((extent+63)/64*64)) return false;
  std::uint32_t seed_scores[64],seed_rows[64];
  // Keep other buckets' leading lanes out of seed selection. The first partial
  // block and the final tail are still scored and screened below.
  const std::size_t seed_first=leading_rows==0?0:quantized_block::kBlockRows;
  const std::size_t scored=(extent-seed_first)/64*64;
  scan(packed+seed_first*dimension,query,dimension,norms+seed_first,
      correction,scored,scores.data()+seed_first,seed_scores,seed_rows);
  const auto tail=quantized_block::best_kernel();
  if (seed_first!=0) tail(packed,query,dimension,norms,correction,scores.data());
  for (std::size_t row=seed_first+scored;row<extent;row+=16) {
    tail(packed+row*dimension,query,dimension,norms+row,correction,scores.data()+row);
  }
  // mask64 reads a complete group even when fewer than four tail blocks were
  // scored. Initialize only those extra lanes; the real/padded blocks above
  // were overwritten by kernels, so no full scratch clear is necessary.
  std::fill(scores.data()+(extent+15)/16*16,
            scores.data()+(extent+63)/64*64,std::uint32_t{0xffffffff});
  struct Seed { std::uint32_t score,row; };
  std::array<Seed,64> seeds;
  for (std::size_t j=0;j<64;++j)
    seeds[j]={seed_scores[j],static_cast<std::uint32_t>(seed_first)+seed_rows[j]};
  const auto seed_less=[](const Seed &a,const Seed &b) {
    return a.score<b.score || (a.score==b.score && a.row<b.row);
  };
  std::nth_element(seeds.begin(),seeds.begin()+capacity,seeds.end(),seed_less);
  for (std::size_t j=0;j<capacity;++j)
    consume_seed(first_row+seeds[j].row-leading_rows);
  std::sort(seeds.begin(),seeds.begin()+capacity,
      [](const Seed &a,const Seed &b){return a.row<b.row;});
  const auto mask=mask_kernel();
  std::size_t seed_index=0;
  for (std::size_t row=0;row<extent;row+=64) {
    const std::size_t count=std::min<std::size_t>(64,extent-row);
    std::uint64_t survivors=mask(scores.data()+row,boundary());
    if (row==0) survivors&=~std::uint64_t{0}<<leading_rows;
    if (count<64) survivors&=(std::uint64_t{1}<<count)-1;
    while (seed_index<capacity && seeds[seed_index].row<row+count) {
      survivors&=~(std::uint64_t{1}<<(seeds[seed_index++].row-row));
    }
    while (survivors!=0) {
      const std::size_t index=row+std::countr_zero(survivors);
      survivors&=survivors-1;
      consume_survivor(first_row+index-leading_rows);
    }
  }
  return true;
}

}  // namespace mvd::quantized_seed
#endif
