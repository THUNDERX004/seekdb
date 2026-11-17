// Standalone correctness checks; no database files or benchmark workload.
// The real caller supplies the published residual envelope and null checks.
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
using Rows = mvd::Table::IndexedRows;

mvd::TableDef prepareDefinition(size_t dimension) {
  mvd::TableDef definition;
  definition.oid = 1;
  definition.name = "quantized_prepare";
  definition.primary_key = 0;
  definition.vector_column = 1;
  definition.columns = {{"id", mvd::ColumnType::BigInt, 0},
                        {"v", mvd::ColumnType::Vector, static_cast<uint32_t>(dimension)}};
  return definition;
}

void requirePrepare(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

uint32_t nextBits(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

struct Fixture {
  static constexpr uint32_t guard_bits = 0x7f7fffffU;
  size_t dimension;
  std::vector<std::vector<float>> storage;
  std::vector<size_t> first;
  Rows rows;

  Fixture(size_t count, size_t axes)
      : dimension(axes), storage(count), first(count) {
    rows.reserve(count);
    for (size_t row = 0; row < count; ++row) {
      storage[row].assign(axes + 32, std::bit_cast<float>(guard_bits));
      first[row] = 9 + row % 7;
      // Exercise unaligned loads without violating float alignment.
      if (reinterpret_cast<uintptr_t>(storage[row].data() + first[row]) % 32 == 0) {
        ++first[row];
      }
      rows.push_back({static_cast<int64_t>(row), static_cast<uint64_t>(row),
                      nullptr, storage[row].data() + first[row]});
    }
  }

  float &at(size_t row, size_t axis) { return storage[row][first[row] + axis]; }

  template <typename Generator>
  void fill(Generator generator) {
    for (size_t row = 0; row < rows.size(); ++row) {
      for (size_t axis = 0; axis < dimension; ++axis) {
        at(row, axis) = generator(row, axis);
      }
    }
  }

  void checkGuards(const std::string &label) const {
    for (size_t row = 0; row < rows.size(); ++row) {
      for (size_t index = 0; index < storage[row].size(); ++index) {
        if (index >= first[row] && index < first[row] + dimension) continue;
        requirePrepare(std::bit_cast<uint32_t>(storage[row][index]) == guard_bits,
                       label + ": input guard changed");
      }
    }
  }
};

// Positive finite IEEE binary32 bit patterns have the same order as their
// magnitudes. This oracle does not use the SIMD/scalar max or abs helpers.
double referenceMaximum(const Rows &rows, size_t dimension) {
  uint32_t maximum = 0;
  for (const auto &row : rows) {
    requirePrepare(row.vector_data != nullptr, "oracle received a null vector");
    for (size_t axis = 0; axis < dimension; ++axis) {
      const uint32_t magnitude =
          std::bit_cast<uint32_t>(row.vector_data[axis]) & 0x7fffffffU;
      requirePrepare(magnitude < 0x7f800000U, "fixture contains a nonfinite float");
      maximum = std::max(maximum, magnitude);
    }
  }
  return static_cast<double>(std::bit_cast<float>(maximum));
}

double referenceScale(const Rows &rows, size_t dimension) {
  const double maximum = referenceMaximum(rows, dimension);
  if (maximum == 0) return 1;
  int exponent = 0;
  std::frexp(maximum, &exponent);
  return std::ldexp(1.0, 7 - exponent);
}

std::pair<long double, long double> referenceBounds(const Rows &rows, size_t dimension) {
  long double minimum = std::numeric_limits<long double>::infinity();
  long double maximum = -minimum;
  for (const auto &row : rows) {
    for (size_t axis = 0; axis < dimension; ++axis) {
      const long double value = row.vector_data[axis];
      requirePrepare(std::isfinite(value), "bounds oracle received a nonfinite value");
      if (value < minimum) minimum = value;
      if (value > maximum) maximum = value;
    }
  }
  return {minimum, maximum};
}

// Selection is checked separately from encoding: code/residual checks below
// always consume the actual published center and step.
std::pair<double, double> referenceCenterStep(const Rows &rows, size_t dimension) {
  const auto [minimum, maximum] = referenceBounds(rows, dimension);
  const double original_step = 1.0 / referenceScale(rows, dimension);
  const double center = static_cast<double>((minimum + maximum) / 2);
  const long double radius = std::max(std::abs(minimum - center),
                                      std::abs(maximum - center));
  if (radius > 0) {
    int exponent = 0;
    std::frexp(radius, &exponent);
    const double centered_step = std::ldexp(1.0, exponent - 7);
    if (centered_step <= original_step / 2) return {center, centered_step};
  }
  return {0, original_step};
}

size_t storedRows(size_t count, bool blocked) {
  return blocked ? (count + 15) / 16 * 16 : count;
}

// The layout oracle follows the public 16-row/four-axis format, and never
// calls a production offset, padding, packing, norm, or rounding helper.
size_t codePosition(size_t row, size_t axis, size_t stride, bool blocked) {
  if (!blocked) return row * stride + axis;
  const size_t block = row / 16;
  const size_t group = axis / 4;
  return block * (16 * stride) + group * 64 + (row % 16) * 4 + axis % 4;
}

struct Oracle {
  std::vector<uint8_t> codes;
  std::vector<uint32_t> norms;
  long double maximum_squared_error = 0;
  long double maximum_rounded_squared_error = 0;
};

Oracle referenceEncode(const Rows &rows, size_t dimension, double scale,
                       size_t stride, bool blocked, double center = 0,
                       const int8_t *observed = nullptr) {
  Oracle result;
  result.codes.assign(storedRows(rows.size(), blocked) * stride, blocked ? 128 : 0);
  result.norms.assign(storedRows(rows.size(), blocked), 0);
  for (size_t row = 0; row < rows.size(); ++row) {
    uint64_t norm = 0;
    long double error = 0, rounded_error = 0;
    for (size_t axis = 0; axis < dimension; ++axis) {
      const float value = rows[row].vector_data[axis];
      const double scaled = (static_cast<double>(value) - center) * scale;
      requirePrepare(std::isfinite(scaled) && std::abs(scaled) < 128,
                     "fixture violates the strict scaled-coordinate contract");
      // Deliberately use std::round, not the SIMD add-half/truncate recipe.
      int code = static_cast<int>(std::clamp(std::round(scaled), -127.0, 127.0));
      const size_t position = codePosition(row, axis, stride, blocked);
      if (observed != nullptr) {
        const int actual = blocked ? int(std::bit_cast<uint8_t>(observed[position])) - 128
                                   : int(observed[position]);
        if (actual != code) {
          // Center subtraction can produce the double immediately inside a
          // half tie. The documented AVX2 add-half can then round away while
          // std::round rounds toward zero. Accept only that adjacent choice,
          // and measure the residual/norm of the code actually emitted.
          const long double half = (static_cast<long double>(actual) + code) / 2;
          const long double difference = std::abs(static_cast<long double>(scaled) - half);
          const double ulp = std::nextafter(std::abs(scaled),
              std::numeric_limits<double>::infinity()) - std::abs(scaled);
          requirePrepare(center != 0 && actual >= -127 && actual <= 127 &&
                             std::abs(actual) == std::abs(code) + 1 &&
                             (actual < 0) == std::signbit(scaled) &&
                             difference > 0 && difference <= ulp,
                         "code disagrees with independent centered rounding oracle");
        }
        code = actual;
      }
      result.codes[position] =
          static_cast<uint8_t>(code + (blocked ? 128 : 0));
      norm += static_cast<uint64_t>(code * code);
      const long double residual = (static_cast<long double>(value) -
          static_cast<long double>(center)) * static_cast<long double>(scale) - code;
      error += residual * residual;
      const long double rounded_residual = static_cast<long double>(scaled) - code;
      rounded_error += rounded_residual * rounded_residual;
    }
    requirePrepare(norm <= std::numeric_limits<uint32_t>::max(),
                   "fixture norm exceeds uint32");
    result.norms[row] = static_cast<uint32_t>(norm);
    result.maximum_squared_error = std::max(result.maximum_squared_error, error);
    result.maximum_rounded_squared_error = std::max(result.maximum_rounded_squared_error,
                                                   rounded_error);
  }
  return result;
}

std::string describe(const Fixture &fixture, const std::string &name, bool blocked) {
  return name + " rows=" + std::to_string(fixture.rows.size()) +
      " dimension=" + std::to_string(fixture.dimension) +
      (blocked ? " blocked" : " row-major");
}

void checkNative(const Fixture &fixture, double scale, const std::string &name,
                 double center = 0) {
#if MVD_QUANTIZED_PREPARE_AVX2
  const auto bounds = mvd::quantized_prepare::bounds(fixture.rows, fixture.dimension);
  const auto expected_bounds = referenceBounds(fixture.rows, fixture.dimension);
  requirePrepare(bounds.first == expected_bounds.first && bounds.second == expected_bounds.second,
                 name + ": min/max differs from the independent bounds oracle");
  const double maximum = mvd::quantized_prepare::maximum_absolute(
      fixture.rows, fixture.dimension);
  requirePrepare(std::bit_cast<uint64_t>(maximum) ==
                     std::bit_cast<uint64_t>(referenceMaximum(fixture.rows, fixture.dimension)),
                 name + ": maximum absolute value is not exact (including +0)");
  using Encode = double (*)(const Rows &, size_t, double, size_t, bool,
                            int8_t *, uint32_t *, double) noexcept;
  struct Variant { const char *name; Encode encode; };
  const Variant variants[] = {
      {"AVX2", &mvd::quantized_prepare::encode_avx2<Rows>},
      {"dispatch", &mvd::quantized_prepare::encode<Rows>},
      {"AVX512", mvd::quantized_prepare::avx512_available()
          ? &mvd::quantized_prepare::encode_avx512<Rows> : nullptr}};
  for (const auto &variant : variants) {
    if (variant.encode == nullptr) continue;
  for (bool blocked : {false, true}) {
    const std::string label = describe(fixture, name + " " + variant.name, blocked);
    // Extra row-major padding also checks that the encoder respects stride.
    const size_t stride = blocked ? (fixture.dimension + 3) / 4 * 4
                                  : fixture.dimension + 3;
    Oracle expected = referenceEncode(
        fixture.rows, fixture.dimension, scale, stride, blocked, center);
    constexpr size_t code_prefix = 67, code_suffix = 71;
    constexpr size_t norm_prefix = 5, norm_suffix = 7;
    constexpr uint8_t byte_guard = 0xa5;
    constexpr uint32_t word_guard = 0xa5c39e71U;
    std::vector<uint8_t> bytes(code_prefix + expected.codes.size() + code_suffix, byte_guard);
    std::fill(bytes.begin() + code_prefix, bytes.end() - code_suffix, blocked ? 128 : 0);
    std::vector<uint32_t> norms(norm_prefix + expected.norms.size() + norm_suffix, word_guard);
    std::fill(norms.begin() + norm_prefix, norms.end() - norm_suffix, 0);
    for (size_t row = 0; row < fixture.rows.size(); ++row) {
      norms[norm_prefix + row] = ~expected.norms[row];
      for (size_t axis = 0; axis < fixture.dimension; ++axis) {
        const size_t offset = codePosition(row, axis, stride, blocked);
        // Every real coordinate must be written, even when its code is zero.
        bytes[code_prefix + offset] = static_cast<uint8_t>(expected.codes[offset] ^ 0xffU);
      }
    }
    const double squared_error = variant.encode(
        fixture.rows, fixture.dimension, scale, stride, blocked,
        reinterpret_cast<int8_t *>(bytes.data() + code_prefix), norms.data() + norm_prefix,
        center);
    if (center != 0) {
      expected = referenceEncode(fixture.rows, fixture.dimension, scale, stride, blocked,
                                 center, reinterpret_cast<const int8_t *>(bytes.data() + code_prefix));
    }
    for (size_t index = 0; index < bytes.size(); ++index) {
      const uint8_t wanted = index >= code_prefix && index < code_prefix + expected.codes.size()
          ? expected.codes[index - code_prefix] : byte_guard;
      if (bytes[index] != wanted) {
        throw std::runtime_error(label + ": code/padding/guard mismatch at byte " +
                                 std::to_string(index));
      }
    }
    for (size_t index = 0; index < norms.size(); ++index) {
      const uint32_t wanted = index >= norm_prefix && index < norm_prefix + expected.norms.size()
          ? expected.norms[index - norm_prefix] : word_guard;
      if (norms[index] != wanted) {
        throw std::runtime_error(label + ": norm/padded-row/guard mismatch at word " +
                                 std::to_string(index));
      }
    }
    requirePrepare(std::isfinite(squared_error) && squared_error >= 0,
                   label + ": invalid raw residual sum");
    // SIMD changes positive-sum association, so the raw double sum need not
    // equal the long-double sum. Test its accuracy here, then test the actual
    // caller's outward envelope below without recreating that implementation.
    const long double allowance = expected.maximum_rounded_squared_error *
        (static_cast<long double>(fixture.dimension) + 16) * 8 *
        std::numeric_limits<double>::epsilon();
    requirePrepare(std::abs(static_cast<long double>(squared_error) -
                            expected.maximum_rounded_squared_error) <= allowance,
                   label + ": raw residual differs from the long-double oracle");
  }
  }
  fixture.checkGuards(name);
#else
  (void)fixture;
  (void)scale;
  (void)name;
  (void)center;
  throw std::runtime_error("native checks requested in a scalar-only build");
#endif
}

void checkPublishedRows(const Rows &rows, size_t dimension, const std::string &label) {
  const size_t stride = rows.codes_blocked ? (dimension + 3) / 4 * 4 : dimension;
  requirePrepare(rows.codes_attempted && rows.code_stride == stride &&
                     std::isfinite(rows.code_step) && rows.code_step > 0 &&
                     std::isfinite(rows.code_center) && rows.code_source == nullptr &&
                     rows.code_first == 0 &&
                     rows.codes.size() == storedRows(rows.size(), rows.codes_blocked) * stride &&
                     rows.code_norms.size() == storedRows(rows.size(), rows.codes_blocked),
                 label + ": incomplete published metadata");
  const auto [center, step] = referenceCenterStep(rows, dimension);
  requirePrepare(rows.code_center == center && rows.code_step == step,
                 label + ": center must be the midpoint and recover at least one code bit");
  const Oracle expected = referenceEncode(rows, dimension, 1.0 / rows.code_step,
      rows.code_stride, rows.codes_blocked, rows.code_center, rows.codes.data());
  requirePrepare(rows.code_norms == expected.norms, label + ": published norms differ");
  for (size_t index = 0; index < expected.codes.size(); ++index) {
    if (std::bit_cast<uint8_t>(rows.codes[index]) != expected.codes[index]) {
      throw std::runtime_error(label + ": published code/padding mismatch at byte " +
                               std::to_string(index));
    }
  }
  const long double rounded_norm = std::sqrt(expected.maximum_rounded_squared_error);
  requirePrepare(std::isfinite(rows.code_error) && rows.code_error >= rounded_norm,
                 label + ": published residual envelope rounds inward");
  const long double relative_slack = (static_cast<long double>(dimension) + 16) *
      64 * std::numeric_limits<double>::epsilon();
  requirePrepare(rows.code_error <= rounded_norm * (1 + relative_slack) +
                     static_cast<long double>(std::numeric_limits<double>::denorm_min()),
                 label + ": published residual envelope is spuriously loose");
  // code_error bounds the rounded centered coordinates. Independently measure
  // true residuals in long double too: |scaled|<128 bounds subtraction error
  // by 2^-45 code units per axis. The query's extra whole unit covers both
  // row and query translation norms, including a rounded-to-zero residual.
  const long double translation = center == 0 ? 0 :
      std::sqrt(static_cast<long double>(dimension)) * std::ldexp(1.0L, -45);
  requirePrepare(2 * translation < 1 &&
                     std::sqrt(expected.maximum_squared_error) <= rows.code_error + translation,
                 label + ": true long-double residual escapes the translation guard");
}

void checkPublished(const Fixture &fixture, const std::string &name) {
  for (bool blocked : {false, true}) {
    const std::string label = describe(fixture, name + " published", blocked);
    mvd::Table table(prepareDefinition(fixture.dimension), "unused-quantized-prepare.dat");
    table.setCacheLimit(32ULL * 1024 * 1024);
    Rows rows;
    // The public caller requires >=256 rows. Repetition retains every source
    // row, its maximum residual, and the original source-buffer lifetime.
    constexpr size_t count = 257;
    rows.reserve(count);
    for (size_t row = 0; row < count; ++row) {
      rows.push_back({static_cast<int64_t>(row), static_cast<uint64_t>(row), nullptr,
                      fixture.rows[row % fixture.rows.size()].vector_data});
    }
    const auto *directory = rows.data();
    requirePrepare(table.prepareQuantizedRows(rows, fixture.dimension, blocked),
                   label + ": caller refused a valid finite fixture");
    requirePrepare(rows.data() == directory && rows.size() == count && rows.codes_blocked == blocked,
                   label + ": published directory/layout changed");
    checkPublishedRows(rows, fixture.dimension, label);
  }
  fixture.checkGuards(name);
}

enum class Pattern { RandomBits, Subnormal, Extremes };

void fillPattern(Fixture &fixture, Pattern pattern, uint32_t seed) {
  constexpr std::array<uint32_t, 16> extremes{{
      0, 0x80000000U, 1, 0x80000001U, 0x007fffffU, 0x807fffffU,
      0x00800000U, 0x80800000U, 0x7f7fffffU, 0xff7fffffU,
      0x7f7ffffeU, 0xff7ffffeU, 0x3f000000U, 0xbf000000U,
      0x3f800001U, 0xbf800001U}};
  fixture.fill([&](size_t row, size_t axis) {
    uint32_t bits = 0;
    if (pattern == Pattern::Extremes) {
      bits = extremes[(row * 7 + axis) % extremes.size()];
    } else if (pattern == Pattern::Subnormal) {
      bits = 1 + nextBits(seed) % 0x007fffffU;
      bits |= nextBits(seed) & 0x80000000U;
    } else {
      do { bits = nextBits(seed); } while ((bits & 0x7f800000U) == 0x7f800000U);
    }
    return std::bit_cast<float>(bits);
  });
  if (pattern == Pattern::Subnormal) {
    fixture.at(0, 0) = std::bit_cast<float>(uint32_t{1});
    fixture.at(fixture.rows.size() - 1, fixture.dimension - 1) =
        std::bit_cast<float>(uint32_t{0x807fffffU});
  }
}

void fillHalfTies(Fixture &fixture) {
  fixture.fill([&](size_t row, size_t axis) {
    const size_t index = row * fixture.dimension + axis;
    float value = static_cast<float>((index / 6) % 128) + 0.5f;
    const size_t neighbor = index / 2 % 3;
    if (neighbor == 0) value = std::nextafter(value, 0.0f);
    if (neighbor == 2) value = std::nextafter(value, std::numeric_limits<float>::infinity());
    uint32_t bits = std::bit_cast<uint32_t>(value);
    if (index % 2) bits |= 0x80000000U;
    return std::bit_cast<float>(bits);
  });
}

void fillScale(Fixture &fixture, int exponent, uint32_t seed) {
  const float largest = exponent == 128 ? std::numeric_limits<float>::max()
      : std::nextafter(std::ldexp(1.0f, exponent), 0.0f);
  const uint32_t largest_bits = std::bit_cast<uint32_t>(largest);
  fixture.fill([&](size_t row, size_t axis) {
    const size_t index = row * fixture.dimension + axis;
    uint32_t magnitude = 0;
    if (index % 10 == 1) {
      magnitude = largest_bits;
    } else if (index % 10 == 2) {
      magnitude = nextBits(seed) % (largest_bits + 1);
    } else if (index % 10 != 0) {
      const double half_tie = static_cast<double>((index / 2) % 128) + 0.5;
      float value = static_cast<float>(std::ldexp(half_tie, exponent - 7));
      if (index % 3 == 0) value = std::nextafter(value, 0.0f);
      if (index % 3 == 2) value = std::nextafter(value, std::numeric_limits<float>::infinity());
      // At the smallest exponents some half ties cannot exist in binary32.
      // Clamp their converted magnitude to this exponent's finite domain.
      magnitude = std::min(std::bit_cast<uint32_t>(value), largest_bits);
    }
    const uint32_t sign = nextBits(seed) & 0x80000000U;
    return std::bit_cast<float>(magnitude | sign);
  });
  fixture.at(fixture.rows.size() - 1, fixture.dimension - 1) = -largest;
}

void checkMaximumLanes() {
#if MVD_QUANTIZED_PREPARE_AVX2
  for (size_t dimension : {16U, 17U, 19U, 23U, 31U, 33U}) {
    for (size_t count : {1U, 17U}) {
      Fixture fixture(count, dimension);
      fixture.fill([](size_t, size_t) { return -1.0f; });
      for (size_t axis = 0; axis < dimension; ++axis) {
        const size_t row = axis % count;
        for (float extremum : {-19.75f, 19.75f}) {
          fixture.at(row, axis) = extremum;
          const auto [minimum, maximum] = mvd::quantized_prepare::bounds(fixture.rows, dimension);
          requirePrepare(minimum == (extremum < 0 ? -19.75 : -1) &&
                             maximum == (extremum > 0 ? 19.75 : -1),
                         "unique min/max lost in a vector lane, row, or scalar tail");
          requirePrepare(mvd::quantized_prepare::maximum_absolute(fixture.rows, dimension) == 19.75,
                         "unique maximum lost in a vector lane, row, or scalar tail");
        }
        fixture.at(row, axis) = -1.0f;
      }
      fixture.checkGuards("maximum lanes");
    }
  }
#endif
}

void checkCenterFixture(const Fixture &fixture, const std::string &name, bool native,
                        double center, double step) {
  requirePrepare(referenceCenterStep(fixture.rows, fixture.dimension) == std::pair{center, step},
                 name + ": fixture did not realize the intended center/bit boundary");
  if (native) checkNative(fixture, 1.0 / step, name, center);
  checkPublished(fixture, name);
}

void checkCenters(bool native) {
  const float largest = std::numeric_limits<float>::max();
  const float previous = std::nextafter(largest, 0.0f);
  const float tiny = std::numeric_limits<float>::denorm_min();
  struct CenterCase {
    const char *name;
    float minimum, maximum;
    double center, step;
  };
  const CenterCase cases[] = {
      {"positive offset", 64, 65, 64.5, std::ldexp(1.0, -7)},
      {"negative offset", -65, -64, -64.5, std::ldexp(1.0, -7)},
      {"exactly one recovered bit", 1, 3, 2, std::ldexp(1.0, -6)},
      {"mixed signs recover one bit", -31, 32, 0.5, 0.25},
      {"smaller radius without a recovered bit", -8, 24, 0, 0.25},
      {"symmetric mixed signs", -32, 32, 0, 0.5},
      {"largest positive adjacent floats", previous, largest,
       (static_cast<double>(previous) + largest) / 2, std::ldexp(1.0, 97)},
      {"largest negative adjacent floats", -largest, -previous,
       -(static_cast<double>(previous) + largest) / 2, std::ldexp(1.0, 97)},
      {"largest opposite signs", -largest, largest, 0, std::ldexp(1.0, 121)},
      {"zero to largest finite", 0, largest, static_cast<double>(largest) / 2,
       std::ldexp(1.0, 120)},
      {"positive subnormals", tiny, 3 * tiny, 2 * static_cast<double>(tiny),
       std::ldexp(1.0, -155)},
      {"negative subnormals", -3 * tiny, -tiny, -2 * static_cast<double>(tiny),
       std::ldexp(1.0, -155)},
      {"mixed subnormals without a recovered bit", -tiny, 3 * tiny, 0,
       std::ldexp(1.0, -154)},
      {"normal/subnormal boundary", std::bit_cast<float>(uint32_t{0x007ffffeU}),
       std::bit_cast<float>(uint32_t{0x00800002U}), std::ldexp(1.0, -126),
       std::ldexp(1.0, -154)}};
  for (const auto &test : cases) {
    Fixture fixture(17, 19);
    fixture.fill([&](size_t row, size_t axis) {
      const long double fraction = static_cast<long double>((row * 5 + axis * 3) % 11 + 1) / 12;
      return static_cast<float>(test.minimum +
          (static_cast<long double>(test.maximum) - test.minimum) * fraction);
    });
    // Both extrema occur at a scalar tail, in different rows.
    fixture.at(0, fixture.dimension - 1) = test.minimum;
    fixture.at(fixture.rows.size() - 1, fixture.dimension - 1) = test.maximum;
    checkCenterFixture(fixture, test.name, native, test.center, test.step);
  }
  for (float value : {1.5f, -1.5f, largest, -largest, tiny, -tiny}) {
    Fixture constant(17, 19);
    constant.fill([&](size_t, size_t) { return value; });
    checkCenterFixture(constant, "constant radius zero keeps center zero", native,
                       0, 1.0 / referenceScale(constant.rows, constant.dimension));
  }
  for (size_t dimension : {16U, 17U, 19U, 23U, 31U, 33U}) {
    for (float sign : {-1.0f, 1.0f}) {
      Fixture tail(17, dimension);
      tail.fill([&](size_t, size_t) { return sign * 64.5f; });
      tail.at(0, dimension - 1) = sign * 64;
      tail.at(16, dimension - 1) = sign * 65;
      checkCenterFixture(tail, "center extrema only in final axis", native,
                         sign * 64.5, std::ldexp(1.0, -7));
    }
  }
  Fixture rounded(17, 19);
  rounded.fill([](size_t row, size_t axis) {
    const float values[] = {-1.984375f, 2.0f, std::ldexp(1.0f, -60),
                            -std::ldexp(1.0f, -60), 0.0f};
    return values[(row + axis) % 5];
  });
  checkCenterFixture(rounded, "centered adjacent double half ties", native,
                     std::ldexp(1.0, -7), std::ldexp(1.0, -6));
  Fixture translated(17, 19);
  translated.fill([](size_t row, size_t axis) {
    const float values[] = {0, 1, 2, std::ldexp(1.0f, -60)};
    return values[(row + axis) % 4];
  });
  checkCenterFixture(translated, "translation residual rounded to zero in double", native,
                     1, std::ldexp(1.0, -6));
  if constexpr (std::numeric_limits<long double>::digits > std::numeric_limits<double>::digits) {
    const Oracle expected = referenceEncode(translated.rows, translated.dimension, 64,
                                            translated.dimension, false, 1);
    requirePrepare(expected.maximum_rounded_squared_error == 0 && expected.maximum_squared_error > 0,
                   "long-double oracle did not retain the translation residual");
  }
}

void checkCenterLifetime() {
  for (bool blocked : {false, true}) {
    Fixture positive(257, 19), negative(257, 19);
    positive.fill([](size_t row, size_t axis) { return 64.0f + float((row + axis) % 17) / 16; });
    negative.fill([](size_t row, size_t axis) { return -32.0f + float((row + axis) % 17) / 8; });
    mvd::Table table(prepareDefinition(19), "unused-quantized-prepare-lifetime.dat");
    table.setCacheLimit(1024 * 1024);
    requirePrepare(table.prepareQuantizedRows(positive.rows, 19, blocked) &&
                       table.prepareQuantizedRows(negative.rows, 19, !blocked),
                   "centered lifetime fixtures were not prepared");
    const auto *positive_directory = positive.rows.data(), *negative_directory = negative.rows.data();
    const auto *positive_codes = positive.rows.codes.data(), *negative_codes = negative.rows.codes.data();
    const auto *positive_norms = positive.rows.code_norms.data(), *negative_norms = negative.rows.code_norms.data();
    for (int reuse = 0; reuse < 8; ++reuse) {
      requirePrepare(table.prepareQuantizedRows(positive.rows, 19, blocked) &&
                         table.prepareQuantizedRows(negative.rows, 19, !blocked) &&
                         positive.rows.code_center == 64.5 && positive.rows.code_step == 1.0 / 128 &&
                         negative.rows.code_center == -31 && negative.rows.code_step == 1.0 / 64 &&
                         positive.rows.codes.data() == positive_codes &&
                         negative.rows.codes.data() == negative_codes &&
                         positive.rows.code_norms.data() == positive_norms &&
                         negative.rows.code_norms.data() == negative_norms,
                     "cache reuse changed the published center, step, or storage");
    }
    positive.rows.swap(negative.rows);
    requirePrepare(positive.rows.data() == negative_directory && negative.rows.data() == positive_directory &&
                       positive.rows.codes.data() == negative_codes && negative.rows.codes.data() == positive_codes &&
                       positive.rows.code_norms.data() == negative_norms && negative.rows.code_norms.data() == positive_norms &&
                       positive.rows.code_center == -31 && positive.rows.code_step == 1.0 / 64 &&
                       negative.rows.code_center == 64.5 && negative.rows.code_step == 1.0 / 128 &&
                       positive.rows.codes_blocked == !blocked && negative.rows.codes_blocked == blocked,
                   "swap separated center/step from its directory, codes, norms, or layout");
    checkPublishedRows(positive.rows, 19, "negative cache after swap");
    checkPublishedRows(negative.rows, 19, "positive cache after swap");
    Rows parked;
    positive.rows.swap(parked);
    requirePrepare(positive.rows.empty() && positive.rows.codes.empty() && positive.rows.code_norms.empty() &&
                       positive.rows.code_center == 0 && positive.rows.code_step == 0 &&
                       positive.rows.code_error == 0 && positive.rows.code_stride == 0 &&
                       !positive.rows.codes_attempted && !positive.rows.codes_blocked &&
                       positive.rows.code_source == nullptr && positive.rows.code_first == 0,
                   "swap with empty directory retained centered cache metadata");
    checkPublishedRows(parked, 19, "centered cache retained by empty swap");
    positive.rows.swap(parked);
    requirePrepare(parked.code_center == 0 && parked.code_step == 0 && parked.empty(),
                   "restoring a centered cache left stale metadata");
    requirePrepare(table.prepareQuantizedRows(positive.rows, 19, !blocked),
                   "swapped centered cache could not be reused");
    checkPublishedRows(positive.rows, 19, "restored centered cache");
    positive.checkGuards("positive center lifetime");
    negative.checkGuards("negative center lifetime");
  }
}

void checkCallerValidation() {
  Fixture fixture(257, 17);
  fixture.fill([](size_t row, size_t axis) { return static_cast<float>((row + axis) % 11 + 1); });
  for (bool blocked : {false, true}) {
    for (size_t missing : {size_t{0}, size_t{128}, size_t{256}}) {
      mvd::Table table(prepareDefinition(fixture.dimension), "unused-quantized-prepare-null.dat");
      table.setCacheLimit(1024 * 1024);
      Rows rows;
      rows.rows = fixture.rows.rows;
      rows.rows[missing].vector_data = nullptr;
      // The header intentionally has no null checks. A missing vector even
      // at the end must be rejected by the caller before it enters a kernel.
      requirePrepare(!table.prepareQuantizedRows(rows, fixture.dimension, blocked),
                     "caller accepted a null vector");
      requirePrepare(rows.codes_attempted && rows.codes.empty() && rows.code_norms.empty() &&
                         rows.code_step == 0 && rows.code_center == 0 && rows.code_error == 0 && rows.code_stride == 0 &&
                         !rows.codes_blocked && rows.code_source == nullptr && rows.code_first == 0,
                     "null vector published a partial preparation");
    }
    Fixture zero(257, 19);
    mvd::Table table(prepareDefinition(zero.dimension), "unused-quantized-prepare-zero.dat");
    table.setCacheLimit(1024 * 1024);
    zero.fill([](size_t row, size_t axis) {
      return std::bit_cast<float>((row + axis) % 2 ? uint32_t{0x80000000U} : uint32_t{0});
    });
    requirePrepare(!table.prepareQuantizedRows(zero.rows, zero.dimension, blocked) &&
                       zero.rows.codes_attempted && zero.rows.codes.empty() &&
                       zero.rows.code_norms.empty() && zero.rows.code_step == 0 && zero.rows.code_center == 0 &&
                       zero.rows.code_error == 0 && zero.rows.code_stride == 0,
                   "all-zero input did not retain the scalar fallback");
  }
  fixture.checkGuards("null caller");
}
}  // namespace

int main() {
  try {
#if MVD_QUANTIZED_PREPARE_AVX2
    __builtin_cpu_init();
    std::cout << (mvd::quantized_prepare::avx512_available()
        ? "Direct AVX512 encoder enabled\n"
        : "SKIP direct AVX512 encoder: CPU/OS state unavailable\n");
#endif
    const bool native = mvd::quantized_prepare::available();
    if (!native) {
      std::cout << "SKIP direct AVX2 preparation: kernel/CPU unavailable; caller checks still run\n";
    }
    constexpr std::array<size_t, 4> counts{{1, 15, 16, 17}};
    if (native) {
      checkMaximumLanes();
      constexpr std::array<size_t, 22> dimensions{{
          7, 8, 9, 16, 17, 19, 31, 33, 63, 64, 65, 127, 128, 129, 255, 257,
          1023, 1025, 4095, 4097, 16383, 16384}};
      for (size_t dimension : dimensions) {
        for (size_t count : counts) {
          for (Pattern pattern : {Pattern::RandomBits, Pattern::Subnormal, Pattern::Extremes}) {
            Fixture fixture(count, dimension);
            const uint32_t seed = 0x6d2b79f5U ^ static_cast<uint32_t>(dimension * 31 + count);
            fillPattern(fixture, pattern, seed);
            checkNative(fixture, referenceScale(fixture.rows, dimension),
                        "finite patterns=" + std::to_string(static_cast<int>(pattern)));
          }
        }
      }
      for (size_t count : counts) {
        Fixture ties(count, 769);
        fillHalfTies(ties);
        checkNative(ties, 1.0, "all signed half ties and adjacent float32 values");
      }
      Fixture zero(17, 19);
      zero.fill([](size_t row, size_t axis) { return (row + axis) % 2 ? -0.0f : 0.0f; });
      checkNative(zero, 1.0, "signed zero");
    }

    // Every possible frexp exponent of a nonzero finite binary32 maximum,
    // hence every scale selected by the caller: 2^155 through 2^-121.
    constexpr std::array<size_t, 4> scale_dimensions{{16, 17, 19, 31}};
    for (int exponent = -148; exponent <= 128; ++exponent) {
      const size_t index = static_cast<size_t>(exponent + 148);
      Fixture fixture(counts[index % counts.size()], scale_dimensions[index % scale_dimensions.size()]);
      fillScale(fixture, exponent, 0xa341316cU + static_cast<uint32_t>(index));
      const double scale = std::ldexp(1.0, 7 - exponent);
      requirePrepare(referenceScale(fixture.rows, fixture.dimension) == scale,
                     "scale sweep failed to realize the requested exponent");
      const std::string name = "scale exponent=" + std::to_string(7 - exponent);
      if (native) checkNative(fixture, scale, name);
      checkPublished(fixture, name);
    }
    // Published envelopes also cover long accumulations and both extreme
    // axis tails, while borrowing only seventeen distinct source buffers.
    for (size_t dimension : {17U, 16383U, 16384U}) {
      Fixture fixture(17, dimension);
      const Pattern pattern = dimension == 17 ? Pattern::Extremes
          : dimension == 16383 ? Pattern::Subnormal : Pattern::RandomBits;
      fillPattern(fixture, pattern, 0x9e3779b9U);
      checkPublished(fixture, "long residual");
    }
    Fixture ties(17, 769);
    fillHalfTies(ties);
    checkPublished(ties, "half ties");
    checkCenters(native);
    checkCenterLifetime();
    checkCallerValidation();
    std::cout << "PASS quantized preparation: exact bounds/codes/norms, layouts/guards, "
                 "all scales, centers/bit recovery/reuse/swap, long-double envelope, caller validation\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL quantized preparation: " << error.what() << '\n';
    return 1;
  }
}
