// Standalone baseline-ISA test; no minivectordb.cpp or build-system dependency.
// Example (one command):
// g++ -std=c++20 -O2 tests/test_quantized_block_scan.cpp -o test_quantized_block_scan
// Run without arguments for correctness only. --benchmark adds a small,
// fixed-work throughput comparison after correctness checks have passed.
#include "../src/quantized_block_scan.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace qb = mvd::quantized_block;

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

struct GuardedBytes {
  static constexpr std::uint8_t kGuard = 0xD3;
  std::vector<std::uint8_t> storage;
  std::size_t begin;
  std::size_t size;

  GuardedBytes(std::size_t bytes, std::size_t misalignment)
      : storage(bytes + 256, kGuard), begin(64), size(bytes) {
    require(misalignment < 64, "invalid fixture alignment");
    const auto address = reinterpret_cast<std::uintptr_t>(storage.data() + begin);
    begin += (64 - address % 64) % 64 + misalignment;
    require(reinterpret_cast<std::uintptr_t>(data()) % 64 == misalignment,
            "fixture alignment mismatch");
  }

  std::uint8_t *data() { return storage.data() + begin; }
  const std::uint8_t *data() const { return storage.data() + begin; }

  bool guards_intact() const {
    return std::all_of(storage.begin(), storage.begin() + begin,
                       [](std::uint8_t value) { return value == kGuard; }) &&
           std::all_of(storage.begin() + begin + size, storage.end(),
                       [](std::uint8_t value) { return value == kGuard; });
  }
};

// Independent oracle: only original signed axes and squared differences.
// It does not consume packed bytes, row norms, correction, or dot products.
std::uint64_t oracle(const std::int8_t *row, const std::int8_t *query,
                     std::size_t dimension) {
  std::uint64_t result = 0;
  for (std::size_t axis = 0; axis < dimension; ++axis) {
    const std::int64_t difference =
        static_cast<std::int64_t>(row[axis]) - query[axis];
    result += static_cast<std::uint64_t>(difference * difference);
  }
  return result;
}

// Different algebra from the header's per-axis correction expression.
std::int64_t independent_correction(const std::int8_t *query,
                                    std::size_t dimension) {
  std::int64_t shifted_norm = 0;
  for (std::size_t axis = 0; axis < dimension; ++axis) {
    const std::int64_t shifted = static_cast<std::int64_t>(query[axis]) + 128;
    shifted_norm += shifted * shifted;
  }
  return shifted_norm - static_cast<std::int64_t>(dimension) * 16384;
}

struct Entry {
  const char *name;
  qb::Kernel kernel;
  std::size_t batches = 0;
};

std::array<Entry, 5> kernels() {
  const qb::CpuFeatures features = qb::cpu_features();
  const auto scalar = qb::kernel_for(qb::KernelKind::scalar);
  const auto avx2 = qb::kernel_for(qb::KernelKind::avx2);
  const auto avx512 = qb::kernel_for(qb::KernelKind::avx512bw);
  const auto vnni = qb::kernel_for(qb::KernelKind::vnni512);
  const auto best = qb::best_kernel();
  require(scalar == &qb::scalar, "scalar resolver mismatch");
  require((avx2 != nullptr) == features.avx2, "AVX2 gate mismatch");
  require((avx512 != nullptr) == features.avx512bw, "AVX512BW gate mismatch");
  require((vnni != nullptr) == features.vnni512, "VNNI512 gate mismatch");
  require(best == (vnni ? vnni : avx512 ? avx512 : avx2 ? avx2 : scalar),
          "best-kernel priority mismatch");
  return {{{"scalar", scalar}, {"AVX2", avx2}, {"AVX512BW", avx512},
           {"VNNI512", vnni}, {"best", best}}};
}

constexpr std::array<const char *, 14> kPatterns{{
    "zeros", "positive-vs-negative", "negative-vs-positive", "equal-positive",
    "equal-negative", "full-int8-query-min", "distinct-rows-zero-query",
    "alternating", "last-axis-only", "four-axis-boundaries", "odd-periods",
    "isolated-row-lanes", "long-sign-runs", "zero-rows-min-query"}};

void fill_pattern(std::vector<std::int8_t> &rows,
                  std::vector<std::int8_t> &query, std::size_t live_rows,
                  std::size_t pattern) {
  const std::size_t dimension = query.size();
  constexpr std::array<int, 7> codes{{127, -127, 126, -1, 0, 1, -126}};
  constexpr std::array<int, 7> queries{{127, -128, 126, -127, 1, -1, 0}};
  std::fill(rows.begin(), rows.end(), 0);
  for (std::size_t axis = 0; axis < dimension; ++axis) {
    int q = 0;
    switch (pattern) {
      case 1: case 4: q = -127; break;
      case 2: case 3: q = 127; break;
      case 5: case 13: q = -128; break;
      case 7: q = axis % 2 ? -127 : 127; break;
      case 8: q = axis + 1 == dimension ? -128 : 0; break;
      case 9: q = (axis / 4) % 2 ? -128 : 127; break;
      case 10: q = queries[(axis * 3 + 1) % queries.size()]; break;
      case 11: q = static_cast<int>(axis % 255) - 127; break;
      case 12: q = axis < dimension / 2 ? 127 : -128; break;
      default: break;
    }
    query[axis] = static_cast<std::int8_t>(q);
    for (std::size_t row = 0; row < live_rows; ++row) {
      int value = 0;
      switch (pattern) {
        case 1: case 3: case 5: value = 127; break;
        case 2: case 4: value = -127; break;
        case 6: value = -127 + static_cast<int>(row) * 16; break;
        case 7:
          value = row % 4 == 0 ? q : row % 4 == 1 ? -q :
                  row % 4 == 2 ? 0 : (axis % 2 ? 126 : -126);
          break;
        case 8:
          if (axis + 1 == dimension) value = -127 + static_cast<int>(row) * 16;
          break;
        case 9:
          value = codes[(axis % 4 + row) % codes.size()];
          break;
        case 10:
          value = codes[(axis + row * 2) % codes.size()];
          break;
        case 11:
          value = axis % 16 == row ? (row % 2 ? -127 : 127) : 0;
          break;
        case 12:
          value = row % 3 == 0 ? 127 : row % 3 == 1 ? -127 :
                  (q > 0 ? -127 : 127);
          break;
        default: break;
      }
      rows[row * dimension + axis] = static_cast<std::int8_t>(value);
    }
  }
}

void check_case(const std::vector<std::int8_t> &rows,
                const std::vector<std::int8_t> &query, std::size_t live_rows,
                const std::string &label, std::size_t alignment,
                std::array<Entry, 5> &entries) {
  const std::size_t dimension = query.size();
  const std::size_t padded = ((dimension + 3) / 4) * 4;
  require(qb::padded_dimension(dimension) == padded, "dimension rounding mismatch");
  require(qb::packed_bytes(padded) == 16 * padded, "packed size mismatch");
  require(qb::valid_padded_dimension(padded), "valid dimension rejected");
  GuardedBytes packed(16 * padded, alignment);
  GuardedBytes padded_query(padded, (alignment * 3 + 1) % 64);
  GuardedBytes norms(16 * sizeof(std::uint32_t), (alignment * 5 + 3) % 64);
  GuardedBytes scores(16 * sizeof(std::uint32_t), (alignment * 7 + 5) % 64);
  std::fill_n(packed.data(), packed.size, std::uint8_t{128});
  std::fill_n(padded_query.data(), padded_query.size, std::uint8_t{0});
  std::memcpy(padded_query.data(), query.data(), dimension);

  // Test-only packer. Each row contributes four consecutive bytes to a group.
  for (std::size_t group = 0; group < padded / 4; ++group) {
    for (std::size_t row = 0; row < live_rows; ++row) {
      for (std::size_t lane = 0; lane < 4; ++lane) {
        const std::size_t axis = group * 4 + lane;
        if (axis < dimension) {
          packed.data()[group * 64 + row * 4 + lane] =
              static_cast<std::uint8_t>(static_cast<int>(rows[row * dimension + axis]) + 128);
        }
      }
    }
  }
  std::array<std::uint64_t, 16> expected{};
  for (std::size_t row = 0; row < 16; ++row) {
    std::uint32_t norm = 0;
    for (std::size_t axis = 0; axis < dimension; ++axis) {
      const std::int32_t value = rows[row * dimension + axis];
      norm += static_cast<std::uint32_t>(value * value);
    }
    std::memcpy(norms.data() + row * sizeof(norm), &norm, sizeof(norm));
    expected[row] = oracle(rows.data() + row * dimension, query.data(), dimension);
    require(expected[row] < (std::uint64_t{1} << 30), "fixture exceeds score bound");
  }
  const auto *query_bytes = reinterpret_cast<const std::int8_t *>(padded_query.data());
  const std::int64_t correction = independent_correction(query.data(), dimension);
  require(qb::query_correction(query_bytes, dimension) == correction,
          "actual-dimension correction mismatch");
  require(qb::query_correction(query_bytes, padded) == correction,
          "padded-dimension correction mismatch");
  const auto packed_before = packed.storage;
  const auto query_before = padded_query.storage;
  const auto norms_before = norms.storage;
  const std::string context = label + ", dimension=" + std::to_string(dimension) +
                              ", live_rows=" + std::to_string(live_rows) +
                              ", alignment=" + std::to_string(alignment);
  for (auto &entry : entries) {
    if (entry.kernel == nullptr) continue;
    // Different initial outputs catch an accidental accumulating API and make
    // determinism a comparison of every score, not merely a checksum.
    for (const std::uint8_t poison : {std::uint8_t{0xA5}, std::uint8_t{0x5A}}) {
      std::fill_n(scores.data(), scores.size, poison);
      entry.kernel(packed.data(), query_bytes, padded, norms.data(), correction,
                   scores.data());
      require(scores.guards_intact(), std::string(entry.name) + " output overrun: " + context);
      for (std::size_t row = 0; row < 16; ++row) {
        std::uint32_t actual = 0;
        std::memcpy(&actual, scores.data() + row * sizeof(actual), sizeof(actual));
        require(actual == expected[row],
                std::string(entry.name) + " mismatch: " + context +
                ", row=" + std::to_string(row) +
                ", expected=" + std::to_string(expected[row]) +
                ", actual=" + std::to_string(actual));
      }
      ++entry.batches;
    }
    require(packed.storage == packed_before && padded_query.storage == query_before &&
                norms.storage == norms_before,
            std::string(entry.name) + " modified an input: " + context);
  }
}

void correctness(std::array<Entry, 5> &entries) {
  for (const auto &entry : entries) {
    std::cout << (entry.kernel ? "RUN " : "SKIP ") << entry.name;
    if (!entry.kernel) std::cout << ": CPU/OS support or compiled ISA unavailable";
    std::cout << '\n';
  }
  for (const std::size_t invalid : {std::size_t{0}, std::size_t{15}, std::size_t{17},
                                   std::size_t{16385}, std::numeric_limits<std::size_t>::max()}) {
    require(!qb::valid_padded_dimension(invalid), "invalid padded dimension accepted");
  }
  constexpr std::array<std::size_t, 11> dimensions{{
      16, 17, 31, 32, 63, 64, 127, 128, 129, 512, 16384}};
  std::mt19937 random(0xB10C16U);
  std::size_t cases = 0;
  for (const std::size_t dimension : dimensions) {
    std::vector<std::int8_t> rows(16 * dimension, 0);
    std::vector<std::int8_t> query(dimension, 0);
    for (std::size_t live_rows = 0; live_rows <= 16; ++live_rows) {
      for (std::size_t pattern = 0; pattern < kPatterns.size(); ++pattern) {
        fill_pattern(rows, query, live_rows, pattern);
        check_case(rows, query, live_rows, kPatterns[pattern], cases % 64, entries);
        ++cases;
      }
    }
    for (std::size_t trial = 0; trial < 16; ++trial) {
      const std::size_t live_rows = random() % 17;
      std::fill(rows.begin(), rows.end(), 0);
      for (auto &q : query) {
        q = static_cast<std::int8_t>(static_cast<int>(random() % 256) - 128);
      }
      for (std::size_t row = 0; row < live_rows; ++row) {
        for (std::size_t axis = 0; axis < dimension; ++axis) {
          rows[row * dimension + axis] =
              static_cast<std::int8_t>(static_cast<int>(random() % 255) - 127);
        }
      }
      check_case(rows, query, live_rows, "seeded-random-" + std::to_string(trial),
                 cases % 64, entries);
      ++cases;
    }
  }
  for (const auto &entry : entries) {
    if (entry.kernel) {
      std::cout << "PASS " << entry.name << ": " << entry.batches << " blocks, "
                << entry.batches * 16 << " independently checked scores\n";
    }
  }
  std::cout << "PASS " << cases << " fixtures; all row tails and byte alignments checked\n";
}

// Exercise the wide mask directly: the score kernels above cover one block,
// while the scan hot path consumes four distinct blocks and eight dot chains.
void check_mask64() {
  const std::array<qb::MaskKernel, 2> entries{{
      qb::mask64_kernel_for(qb::KernelKind::scalar),
      qb::mask64_kernel_for(qb::KernelKind::vnni512)}};
  std::mt19937 random(0x8ACC0064U);
  std::size_t masks = 0;
  for (const std::size_t dimension : {16U, 17U, 20U, 24U, 28U, 31U, 32U,
                                      127U, 128U, 129U, 512U, 16384U}) {
    const std::size_t padded = qb::padded_dimension(dimension);
    for (unsigned pattern = 0; pattern < 3; ++pattern) {
      GuardedBytes packed(64 * padded, (dimension + pattern) % 64);
      GuardedBytes query_bytes(padded, 3);
      GuardedBytes norms(64 * sizeof(std::uint32_t), 5);
      std::fill_n(packed.data(), packed.size, std::uint8_t{128});
      std::fill_n(query_bytes.data(), padded, std::uint8_t{0});
      std::vector<std::int8_t> query(dimension);
      std::vector<std::int8_t> row(dimension);
      for (std::size_t axis = 0; axis < dimension; ++axis) {
        const int value = pattern == 0
            ? static_cast<int>(random() % 256) - 128
            : pattern == 1 ? ((axis / 4) % 2 ? -128 : 127)
                           : (axis + 1 == dimension ? -128 : 0);
        query[axis] = static_cast<std::int8_t>(value);
      }
      std::memcpy(query_bytes.data(), query.data(), dimension);
      std::array<std::uint64_t, 64> expected{};
      for (std::size_t index = 0; index < 64; ++index) {
        std::uint32_t norm = 0;
        for (std::size_t axis = 0; axis < dimension; ++axis) {
          const int value = pattern == 0
              ? static_cast<int>(random() % 255) - 127
              : pattern == 1 ? ((axis / 4 + index) % 2 ? -127 : 127)
                             : (axis + 1 == dimension ? static_cast<int>(index) - 32 : 0);
          row[axis] = static_cast<std::int8_t>(value);
          norm += static_cast<std::uint32_t>(value * value);
          packed.data()[(index / 16) * 16 * padded +
                        (axis / 4) * 64 + (index % 16) * 4 + axis % 4] =
              static_cast<std::uint8_t>(value + 128);
        }
        std::memcpy(norms.data() + index * sizeof(norm), &norm, sizeof(norm));
        expected[index] = oracle(row.data(), query.data(), dimension);
      }
      const auto packed_before = packed.storage;
      const auto query_before = query_bytes.storage;
      const auto norms_before = norms.storage;
      const auto correction = independent_correction(query.data(), dimension);
      const auto *q = reinterpret_cast<const std::int8_t *>(query_bytes.data());
      std::vector<std::uint32_t> boundaries{0, 1, 0x7fffffffU, 0x80000000U,
                                           std::numeric_limits<std::uint32_t>::max()};
      for (const std::size_t index : {0U, 15U, 16U, 31U, 32U, 47U, 48U, 63U}) {
        const auto score = static_cast<std::uint32_t>(expected[index]);
        boundaries.push_back(score);
        if (score != 0) boundaries.push_back(score - 1);
        boundaries.push_back(score + 1);
      }
      for (const auto boundary : boundaries) {
        std::uint64_t wanted = 0;
        for (std::size_t index = 0; index < 64; ++index) {
          wanted |= std::uint64_t{expected[index] <= boundary} << index;
        }
        for (const auto kernel : entries) {
          if (kernel == nullptr) continue;
          require(kernel(packed.data(), q, padded, norms.data(), correction, boundary) == wanted,
                  "wide mask mismatch: dimension=" + std::to_string(dimension) +
                      ", pattern=" + std::to_string(pattern) +
                      ", boundary=" + std::to_string(boundary));
          ++masks;
        }
      }
      require(packed.storage == packed_before && query_bytes.storage == query_before &&
                  norms.storage == norms_before,
              "wide mask modified an input");
    }
  }
  std::cout << "PASS " << masks
            << " wide masks: independent scores, alternating chains, four-axis tails and inclusive cutoffs\n";
}

// This intentionally measures prepacked, cache-resident blocks. Packing,
// allocation and database cache/lifetime behavior are outside this experiment.
void benchmark(const std::array<Entry, 5> &entries) {
  constexpr std::size_t dimension = 128;
  constexpr std::size_t blocks = 8;
  constexpr std::size_t repetitions = 4096;
  std::vector<std::uint8_t> packed(blocks * 16 * dimension);
  std::array<std::array<std::uint32_t, 16>, blocks> norms{};
  std::array<std::int8_t, dimension> query{};
  std::mt19937 random(0xFA57B10CU);
  for (auto &q : query) q = static_cast<std::int8_t>(static_cast<int>(random() % 255) - 127);
  for (std::size_t block = 0; block < blocks; ++block) {
    for (std::size_t group = 0; group < dimension / 4; ++group) {
      for (std::size_t row = 0; row < 16; ++row) {
        for (std::size_t lane = 0; lane < 4; ++lane) {
          const int code = static_cast<int>(random() % 255) - 127;
          packed[block * 16 * dimension + group * 64 + row * 4 + lane] =
              static_cast<std::uint8_t>(code + 128);
          norms[block][row] += static_cast<std::uint32_t>(code * code);
        }
      }
    }
  }
  const auto correction = qb::query_correction(query.data(), dimension);
  std::array<std::uint32_t, 16> scores{};
  std::cout << "BENCH prepacked " << blocks << " blocks, dimension=" << dimension
            << ", repetitions=" << repetitions << " (packing excluded)\n";
  for (const auto &entry : entries) {
    if (!entry.kernel || std::string(entry.name) == "best") continue;
    std::uint64_t checksum = 0;
    for (std::size_t block = 0; block < blocks; ++block) {
      entry.kernel(packed.data() + block * 16 * dimension, query.data(), dimension,
                   norms[block].data(), correction, scores.data());
      checksum += scores[block];
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t repeat = 0; repeat < repetitions; ++repeat) {
      for (std::size_t block = 0; block < blocks; ++block) {
        entry.kernel(packed.data() + block * 16 * dimension, query.data(), dimension,
                     norms[block].data(), correction, scores.data());
        checksum += scores[(repeat + block) % 16];
      }
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const double scanned = static_cast<double>(repetitions * blocks * 16);
    std::cout << "BENCH " << entry.name << ": " << std::fixed << std::setprecision(2)
              << seconds * 1e9 / scanned << " ns/row, "
              << scanned / seconds / 1e6 << " Mrows/s, checksum=" << checksum << '\n';
  }
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const bool run_benchmark = argc == 2 && std::string(argv[1]) == "--benchmark";
    if (argc != 1 && !run_benchmark) {
      std::cerr << "Usage: test_quantized_block_scan [--benchmark]\n";
      return 2;
    }
    auto entries = kernels();
    correctness(entries);
    check_mask64();
    if (run_benchmark) benchmark(entries);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL quantized block scan: " << error.what() << '\n';
    return 1;
  }
}
