// Standalone baseline-ISA test. The oracle uses original signed codes and
// squared differences, never packed bytes, row norms, or a production kernel.
#include "../src/quantized_block_scan.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qb = mvd::quantized_block;

namespace {

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}

struct Entry {
  const char *name;
  qb::MaskKernel kernel;
  std::size_t width;
  std::size_t masks = 0;
};

std::vector<Entry> entries() {
  std::vector<Entry> result{
      {"scalar", qb::mask_kernel_for(qb::KernelKind::scalar), 16},
      {"scalar", qb::mask64_kernel_for(qb::KernelKind::scalar), 64}};
#ifndef MVD_FORCE_SCALAR
  for (const auto &[kind, name] : std::array{
           std::pair{qb::KernelKind::avx2, "AVX2"},
           std::pair{qb::KernelKind::avx512bw, "AVX512BW"},
           std::pair{qb::KernelKind::vnni512, "VNNI"}}) {
    const auto small = qb::mask_kernel_for(kind);
    if (small != nullptr) result.push_back({name, small, 16});
    else std::cout << "SKIP " << name << "16: unsupported CPU/OS\n";
    const auto wide = qb::mask64_kernel_for(kind);
    if (wide != nullptr) result.push_back({name, wide, 64});
  }
#endif
  return result;
}

void fixture(std::vector<Entry> &kernels, std::mt19937 &random,
             std::size_t dimension, unsigned pattern, std::size_t offset) {
  constexpr std::size_t count = 64;
  const std::size_t stride = qb::padded_dimension(dimension);
  std::vector<std::int8_t> original(count * dimension);
  std::vector<std::int8_t> query_storage(stride + offset, 0);
  auto *query = query_storage.data() + offset;
  std::vector<std::uint8_t> packed_storage(count * stride + offset, 128);
  auto *packed = packed_storage.data() + offset;
  std::vector<std::uint8_t> norm_storage(count * sizeof(std::uint32_t) + offset, 0);
  auto *norms = norm_storage.data() + offset;
  std::array<std::uint64_t, count> expected{};
  for (std::size_t axis = 0; axis < dimension; ++axis) {
    int q = static_cast<int>(random() % 256U) - 128;
    if (pattern == 0 || pattern == 4) q = 0;
    if (pattern == 1 || pattern == 2) q = -128;
    if (pattern == 3) q = axis % 2 ? -128 : 127;
    if (pattern == 5) q = axis + 1 == dimension ? 127 : 0;
    query[axis] = static_cast<std::int8_t>(q);
    for (std::size_t row = 0; row < count; ++row) {
      int code = static_cast<int>(random() % 255U) - 127;
      if (pattern == 0) code = 0;
      if (pattern == 1) code = row % 2 ? -127 : 127;
      if (pattern == 2) code = -127;
      if (pattern == 3) code = (axis + row) % 2 ? 127 : -127;
      if (pattern == 4) code = static_cast<int>(row % 15) - 7;
      if (pattern == 5) code = axis + 1 == dimension
          ? static_cast<int>(row) - 32 : 0;
      original[row * dimension + axis] = static_cast<std::int8_t>(code);
    }
  }
  std::int64_t shifted_query_norm = 0;
  for (std::size_t axis = 0; axis < stride; ++axis) {
    const std::int64_t shifted = static_cast<std::int64_t>(query[axis]) + 128;
    shifted_query_norm += shifted * shifted;
  }
  const std::int64_t correction = shifted_query_norm -
      static_cast<std::int64_t>(stride) * 16384;
  for (std::size_t row = 0; row < count; ++row) {
    std::uint32_t norm = 0;
    for (std::size_t axis = 0; axis < dimension; ++axis) {
      const std::int64_t code = original[row * dimension + axis];
      const std::int64_t difference = code - query[axis];
      expected[row] += static_cast<std::uint64_t>(difference * difference);
      norm += static_cast<std::uint32_t>(code * code);
      const std::size_t index = row / 16 * 16 * stride +
          axis / 4 * 64 + row % 16 * 4 + axis % 4;
      packed[index] = static_cast<std::uint8_t>(code + 128);
    }
    std::memcpy(norms + row * sizeof(norm), &norm, sizeof(norm));
  }
  std::vector<std::uint32_t> thresholds{
      0, 1, 0x7fffffffU, 0x80000000U, std::numeric_limits<std::uint32_t>::max()};
  for (const std::size_t row : {std::size_t{0}, std::size_t{15},
                               std::size_t{32}, std::size_t{63}}) {
    const auto score = static_cast<std::uint32_t>(expected[row]);
    thresholds.push_back(score);
    if (score != 0) thresholds.push_back(score - 1);
    thresholds.push_back(score + 1);
  }
  for (auto &entry : kernels) {
    for (const auto boundary : thresholds) {
      for (std::size_t first = 0; first < count; first += entry.width) {
        std::uint64_t oracle = 0;
        for (std::size_t row = 0; row < entry.width; ++row) {
          if (expected[first + row] <= boundary) oracle |= std::uint64_t{1} << row;
        }
        const auto mask = entry.kernel(packed + first * stride, query, stride,
            norms + first * sizeof(std::uint32_t), correction, boundary);
        require(mask == oracle, std::string(entry.name) + " mask mismatch: width=" +
            std::to_string(entry.width) + " dimension=" + std::to_string(dimension) +
            " boundary=" + std::to_string(boundary));
        ++entry.masks;
        // Independent expected membership for shared-bucket heads and tails.
        for (std::size_t lane = 0; lane < 16; ++lane) {
          for (const std::size_t wanted : {std::size_t{1}, std::size_t{15},
                                          std::size_t{16}, std::size_t{63},
                                          std::size_t{64}}) {
            const std::size_t length = std::min(wanted, entry.width - lane);
            std::uint64_t sliced = mask >> lane;
            if (length < 64) sliced &= (std::uint64_t{1} << length) - 1;
            std::uint64_t expected_slice = 0;
            for (std::size_t row = 0; row < length; ++row) {
              if (expected[first + lane + row] <= boundary) {
                expected_slice |= std::uint64_t{1} << row;
              }
            }
            require(sliced == expected_slice, "shared-bucket slice mismatch");
          }
        }
      }
    }
  }
}

}  // namespace

int main() {
  try {
    auto kernels = entries();
    std::mt19937 random(0x3C9182E7U);
    for (const std::size_t dimension : {16U, 17U, 19U, 31U, 63U, 64U, 65U,
                                       127U, 128U, 129U, 255U, 512U,
                                       1023U, 4095U, 16384U}) {
      for (unsigned pattern = 0; pattern < 9; ++pattern) {
        fixture(kernels, random, dimension, pattern, pattern % 2 ? 1 : 3);
      }
    }
    for (const auto &entry : kernels) {
      std::cout << "PASS " << entry.name << entry.width << ": " << entry.masks
                << " independently checked masks (" << entry.masks * entry.width
                << " code distances), inclusive boundaries and shared slices\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
