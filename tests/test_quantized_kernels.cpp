// Standalone correctness harness. Compile for the baseline ISA: the included
// kernels have their own target attributes and are called only after CPU/OS
// checks. No database instance, files, timing, or benchmark workload is used.
#include <random>

#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

#define MVD_TEST_QUANTIZED_KERNELS
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace mvd {

struct QuantizedKernelTest {
  using Kernel = void (*)(const int8_t *, const int8_t *, size_t, uint32_t *,
                         const uint32_t *, int64_t);

  // The expected result never uses norms, unsigned-byte bias, dot products,
  // SIMD lanes, or any production distance helper.
  static uint64_t squaredDifference(const int8_t *row, const int8_t *query,
                                    size_t dimension) {
    uint64_t total = 0;
    for (size_t axis = 0; axis < dimension; ++axis) {
      const int64_t difference = static_cast<int64_t>(row[axis]) - query[axis];
      total += static_cast<uint64_t>(difference * difference);
    }
    return total;
  }

#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
  static uint64_t enabledXstate() {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
      return 0;
    }
    // Execute XGETBV only if XSAVE, OSXSAVE and AVX are all advertised.
    constexpr unsigned int required = (1U << 26) | (1U << 27) | (1U << 28);
    if ((ecx & required) != required) {
      return 0;
    }
    uint32_t low = 0, high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<uint64_t>(high) << 32) | low;
  }
#endif

  static int run() {
#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    const uint64_t xstate = enabledXstate();
    const bool avx2 = (xstate & 0x6U) == 0x6U &&
                     __builtin_cpu_supports("avx") &&
                     __builtin_cpu_supports("avx2");
    // AVX-512 also needs opmask, upper ZMM and the high ZMM registers enabled
    // by the OS, as well as every ISA used by the called target function.
    const bool avx512 = avx2 && (xstate & 0xE6U) == 0xE6U &&
                       __builtin_cpu_supports("avx512f") &&
                       __builtin_cpu_supports("avx512bw");
    const bool vnni = avx512 && __builtin_cpu_supports("avx512vnni");
    struct Entry {
      const char *name;
      Kernel kernel;
      bool supported;
      size_t batches = 0;
    };
    std::array<Entry, 3> kernels{{
        {"AVX2", &Database::quantizedDistanceBatchAvx2, avx2},
        {"AVX512BW", &Database::quantizedDistanceBatchAvx512, avx512},
        {"VNNI", &Database::quantizedDistanceBatchVnni, vnni},
    }};
    size_t executed = 0;
    for (const auto &entry : kernels) {
      if (entry.supported) {
        ++executed;
        std::cout << "RUN " << entry.name << '\n';
      } else {
        std::cout << "SKIP " << entry.name
                  << ": required CPU features or OS register state unavailable\n";
      }
    }
    if (executed == 0) {
      std::cout << "SKIP quantized kernel checks: no supported native kernels\n";
      return 0;
    }

    const auto check = [&](const int8_t *rows, const int8_t *query,
                           size_t dimension, const std::string &label) {
      const std::vector<int8_t> zero(dimension, 0);
      std::array<uint64_t, 4> expected{};
      std::array<uint32_t, 4> norms{};
      for (size_t row = 0; row < 4; ++row) {
        expected[row] = squaredDifference(rows + row * dimension, query, dimension);
        const uint64_t norm = squaredDifference(rows + row * dimension,
                                               zero.data(), dimension);
        if (norm > std::numeric_limits<uint32_t>::max() ||
            expected[row] > std::numeric_limits<uint32_t>::max()) {
          throw std::runtime_error("fixture exceeds the uint32 score contract");
        }
        norms[row] = static_cast<uint32_t>(norm);
      }
      // Independent derivation: ||q + 128||^2 - dimension * 128^2.
      // Keep the subtraction signed: all-negative queries need a negative
      // correction. Do not use the implementation's per-axis expression.
      int64_t shifted_norm = 0;
      for (size_t axis = 0; axis < dimension; ++axis) {
        const int64_t shifted = static_cast<int64_t>(query[axis]) + 128;
        shifted_norm += shifted * shifted;
      }
      const int64_t correction = shifted_norm - static_cast<int64_t>(dimension) * 16384;
      for (auto &entry : kernels) {
        if (!entry.supported) {
          continue;
        }
        constexpr uint32_t guard = 0xA5C39E71U;
        constexpr uint32_t unwritten = std::numeric_limits<uint32_t>::max();
        std::array<uint32_t, 6> output{{guard, unwritten, unwritten,
                                      unwritten, unwritten, guard}};
        entry.kernel(rows, query, dimension, output.data() + 1,
                     norms.data(), correction);
        if (output.front() != guard || output.back() != guard) {
          throw std::runtime_error(std::string(entry.name) + " overwrote score guards: " +
                                   label + ", dimension=" + std::to_string(dimension));
        }
        for (size_t row = 0; row < 4; ++row) {
          if (static_cast<uint64_t>(output[row + 1]) != expected[row]) {
            throw std::runtime_error(
                std::string(entry.name) + " mismatch: " + label +
                ", dimension=" + std::to_string(dimension) +
                ", row=" + std::to_string(row) +
                ", expected=" + std::to_string(expected[row]) +
                ", actual=" + std::to_string(output[row + 1]) +
                ", norm=" + std::to_string(norms[row]) +
                ", correction=" + std::to_string(correction));
          }
        }
        ++entry.batches;
      }
    };

    constexpr std::array<size_t, 12> dimensions{{
        16, 17, 31, 32, 63, 64, 65, 127, 128, 129, 16383, 16384}};
    constexpr std::array<const char *, 9> patterns{{
        "zeros", "positive-versus-negative", "negative-versus-positive",
        "equal-positive", "equal-negative", "distinct-rows-zero-query",
        "alternating", "last-axis-only", "block-boundaries"}};
    constexpr std::array<int, 4> row_values{{-127, -1, 1, 127}};
    std::mt19937 random(0x51A7C0DEU);
    const auto random_code = [&]() {
      return static_cast<int8_t>(static_cast<int>(random() % 255U) - 127);
    };
    for (const size_t dimension : dimensions) {
      // Offset both byte arrays so the kernels must accept unaligned input.
      std::vector<int8_t> row_storage(4 * dimension + 2, 37);
      std::vector<int8_t> query_storage(dimension + 2, -43);
      int8_t *rows = row_storage.data() + 1;
      int8_t *query = query_storage.data() + 1;
      for (size_t pattern = 0; pattern < patterns.size(); ++pattern) {
        for (size_t axis = 0; axis < dimension; ++axis) {
          int q = 0;
          if (pattern == 1 || pattern == 4) q = -127;
          if (pattern == 2 || pattern == 3) q = 127;
          if (pattern == 6) q = axis % 2 ? -127 : 127;
          if (pattern == 7 && axis + 1 == dimension) q = 7;
          if (pattern == 8) q = (axis / 32) % 2 ? -126 : 127;
          query[axis] = static_cast<int8_t>(q);
          for (size_t row = 0; row < 4; ++row) {
            int value = 0;
            if (pattern == 1 || pattern == 3) value = 127;
            if (pattern == 2 || pattern == 4) value = -127;
            if (pattern == 5) value = row_values[row];
            if (pattern == 6) {
              value = row < 2 ? (row == 0 ? q : -q)
                              : (row == 2 ? 0 : (axis % 2 ? 126 : -126));
            }
            if (pattern == 7 && axis + 1 == dimension) value = row_values[row];
            if (pattern == 8) {
              value = ((axis + row * 17) / (row + 1)) % 2 ? -127 : 127;
            }
            rows[row * dimension + axis] = static_cast<int8_t>(value);
          }
        }
        check(rows, query, dimension, patterns[pattern]);
      }
      for (size_t trial = 0; trial < 128; ++trial) {
        for (size_t axis = 0; axis < dimension; ++axis) {
          query[axis] = random_code();
          for (size_t row = 0; row < 4; ++row) {
            rows[row * dimension + axis] = random_code();
          }
        }
        check(rows, query, dimension, "seeded-random-" + std::to_string(trial));
      }

      if (dimension == 16384) {
        // Odd periods cross every lane/block boundary. Large biased dot sums,
        // negative corrections, and cancellation expose narrow accumulators.
        constexpr std::array<int, 7> repeated{{127, -127, 126, -126, 1, -1, 0}};
        for (size_t trial = 0; trial < 4; ++trial) {
          for (size_t axis = 0; axis < dimension; ++axis) {
            const int q = trial == 0 ? 127 : trial == 1 ? -127 :
                          trial == 2 ? (axis % 2 ? -127 : 127) :
                          repeated[(axis * 3 + 1) % repeated.size()];
            query[axis] = static_cast<int8_t>(q);
            for (size_t row = 0; row < 4; ++row) {
              rows[row * dimension + axis] = static_cast<int8_t>(
                  repeated[(axis + row * 2 + trial) % repeated.size()]);
            }
          }
          check(rows, query, dimension, "overflow-repeat-" + std::to_string(trial));
        }
      }
    }
    for (const auto &entry : kernels) {
      if (entry.supported) {
        std::cout << "PASS " << entry.name << ": " << entry.batches
                  << " four-row batches, " << entry.batches * 4
                  << " independently checked scores\n";
      }
    }
    std::cout << "PASS native quantized kernels: executed=" << executed
              << ", skipped=" << kernels.size() - executed << '\n';
    return 0;
#else
    for (const char *name : {"AVX2", "AVX512BW", "VNNI"}) {
      std::cout << "SKIP " << name << ": requires GNU/Clang x86 kernel build\n";
    }
    return 0;
#endif
  }
};

}  // namespace mvd

int main() {
  try {
    return mvd::QuantizedKernelTest::run();
  } catch (const std::exception &error) {
    std::cerr << "FAIL quantized kernel correctness: " << error.what() << '\n';
    return 1;
  }
}
