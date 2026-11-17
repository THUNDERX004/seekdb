#include "../src/quantized_seed_scan.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <cstdlib>
#include <new>
#include <random>
#include <stdexcept>
#include <vector>

static size_t fail_bytes = 0;
static size_t allocation_failures = 0;
void *operator new(size_t bytes) {
  if (bytes == fail_bytes) {
    ++allocation_failures;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(bytes)) return memory;
  throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, size_t) noexcept { std::free(memory); }
void *operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory, size_t) noexcept { std::free(memory); }

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

// The oracle reads the original signed row, independently of the packed
// layout, row norms, SIMD score calculation and saved-score mask.
static uint32_t integer_squared(const int8_t *row, const int8_t *query,
                                size_t dimension) {
  uint64_t squared = 0;
  for (size_t axis = 0; axis < dimension; ++axis) {
    const int64_t difference = int64_t(row[axis]) - int64_t(query[axis]);
    squared += static_cast<uint64_t>(difference * difference);
  }
  require(squared <= std::numeric_limits<uint32_t>::max(), "oracle overflow");
  return static_cast<uint32_t>(squared);
}

struct BucketFixture {
  static constexpr uint32_t guard = 0xdeadbeef;
  size_t dimension, rows, leading, code_first, first_row, padded_rows;
  int64_t correction = 0;
  std::vector<int8_t> vectors, query;
  std::vector<uint8_t> packed;
  std::vector<uint32_t> norms, expected;

  BucketFixture(size_t dimension, size_t rows, size_t leading,
                size_t code_first, size_t first_row, int pattern)
      : dimension(dimension), rows(rows), leading(leading),
        code_first(code_first), first_row(first_row),
        padded_rows((leading + rows + 15) / 16 * 16),
        vectors(rows * dimension), query(dimension + 2, 85),
        packed((code_first + padded_rows) * dimension + 2, 77),
        norms(code_first + padded_rows + 2, guard), expected(rows) {
    std::mt19937 random(static_cast<uint32_t>(20261117 + rows + leading));
    for (size_t axis = 0; axis < dimension; ++axis) {
      const int q = pattern == 0 ? static_cast<int>(random() % 256) - 128
          : axis % 2 == 0 ? -128 : 127;
      query[axis + 1] = static_cast<int8_t>(q);
      correction += int64_t(q) * q + 256 * int64_t(q);
    }
    for (size_t row = 0; row < rows; ++row) {
      for (size_t axis = 0; axis < dimension; ++axis) {
        int value = pattern == 0 ? static_cast<int>(random() % 255) - 127
            : pattern == 1 ? (axis % 2 == 0 ? 21 : -23)
            : ((row + axis) % 3 == 0 ? -127 : 127);
        // Prefix/tail winners cannot become seeds when they fall outside the
        // complete 64-row scan. They must still survive a finite boundary.
        if (pattern != 1 && (row == 0 || row + 1 == rows))
          value = std::max(-127, static_cast<int>(query[axis + 1]));
        vectors[row * dimension + axis] = static_cast<int8_t>(value);
      }
      expected[row] = integer_squared(vectors.data() + row * dimension,
                                     query.data() + 1, dimension);
    }
    for (size_t physical = 0; physical < padded_rows; ++physical) {
      uint32_t norm = 0;
      for (size_t axis = 0; axis < dimension; ++axis) {
        // Padding deliberately resembles the query more closely than most
        // real rows, so an unmasked lane would be an attractive candidate.
        const int value = physical >= leading && physical < leading + rows
            ? vectors[(physical - leading) * dimension + axis]
            : std::max(-127, static_cast<int>(query[axis + 1]));
        norm += value * value;
        packed[1 + code_first * dimension + (physical / 16) * 16 * dimension +
               (axis / 4) * 64 + (physical % 16) * 4 + axis % 4] =
            static_cast<uint8_t>(value + 128);
      }
      norms[1 + code_first + physical] = norm;
    }
  }

  const uint8_t *codes() const { return packed.data() + 1 + code_first * dimension; }
  const uint32_t *row_norms() const { return norms.data() + 1 + code_first; }
};

static void check_bucket_visit(const BucketFixture &fixture) {
  using namespace mvd::quantized_seed;
  // Reuse across different bucket lengths, leading lanes, queries and widths.
  static Scratch reused;
  const auto packed_before = fixture.packed;
  const auto norms_before = fixture.norms;
  const auto query_before = fixture.query;
  const bool eligible = fixture.rows >= 512 && fixture.rows <= 65536;
  std::array<size_t, 64> candidates{};
  if (eligible) {
    const size_t seed_first = fixture.leading == 0 ? 0 : 16;
    const size_t seed_end = seed_first +
        (fixture.leading + fixture.rows - seed_first) / 64 * 64;
    for (size_t lane = 0; lane < candidates.size(); ++lane) {
      size_t best = seed_first + lane - fixture.leading;
      for (size_t physical = seed_first + lane + 64; physical < seed_end;
           physical += 64) {
        const size_t row = physical - fixture.leading;
        if (fixture.expected[row] < fixture.expected[best]) best = row;
      }
      candidates[lane] = best;
    }
    std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
      return fixture.expected[a] < fixture.expected[b] ||
          (fixture.expected[a] == fixture.expected[b] && a < b);
    });
  }
  for (size_t capacity : {size_t{1}, size_t{10}, size_t{32}}) {
    std::vector<uint8_t> selected(fixture.rows);
    if (eligible)
      for (size_t j = 0; j < capacity; ++j) selected[candidates[j]] = 1;
    std::vector<uint32_t> visits(fixture.rows + 2, BucketFixture::guard);
    size_t seeds = 0, survivors = 0, boundaries = 0, heap_size = 0;
    bool valid_indices = true, valid_seeds = true;
    uint32_t above = 0;
    std::array<uint32_t, 32> heap{};
    const auto reset = [&](uint32_t initial) {
      std::fill(visits.begin() + 1, visits.end() - 1, 0);
      seeds = survivors = boundaries = heap_size = 0;
      valid_indices = valid_seeds = true;
      above = initial;
    };
    const auto consume = [&](size_t index, bool seed, bool shrinking) {
      if (seed) ++seeds; else ++survivors;
      if (index < fixture.first_row || index - fixture.first_row >= fixture.rows) {
        valid_indices = false;
        return;
      }
      const size_t row = index - fixture.first_row;
      if (seed && (!selected[row] || survivors != 0)) valid_seeds = false;
      ++visits[row + 1];
      if (shrinking) {
        const uint32_t score = fixture.expected[row];
        if (heap_size < capacity) heap[heap_size++] = score;
        else {
          auto worst = std::max_element(heap.begin(), heap.begin() + heap_size);
          if (score < *worst) *worst = score;
        }
        if (heap_size == capacity)
          above = *std::max_element(heap.begin(), heap.begin() + heap_size);
      }
    };
    const auto run = [&](uint32_t initial, bool shrinking, Scratch *scratch=nullptr) {
      reset(initial);
      const bool accepted = visit(fixture.codes(), fixture.query.data() + 1,
          fixture.dimension, fixture.row_norms(), fixture.correction,
          fixture.rows, fixture.first_row, capacity,
          [&](size_t index) { consume(index, true, shrinking); },
          [&](size_t index) { consume(index, false, shrinking); },
          [&] { ++boundaries; return above; }, fixture.leading, scratch);
      require(accepted == eligible, "real-row eligibility gate differs");
      require(valid_indices && valid_seeds, "padding, seed origin or index translation differs");
      require(visits.front() == BucketFixture::guard &&
                  visits.back() == BucketFixture::guard,
              "callback sentinel overwritten");
      if (!eligible) {
        require(seeds == 0 && survivors == 0 && boundaries == 0,
                "ineligible bucket invoked a callback");
        return;
      }
      require(seeds == capacity && boundaries != 0, "missing seeds or screening");
      for (size_t row = 0; row < fixture.rows; ++row) {
        require(visits[row + 1] <= 1, "real row visited more than once");
        if (shrinking) {
          require(!(selected[row] || fixture.expected[row] <= above) ||
                      visits[row + 1] == 1,
                  "shrinking boundary lost a seed, tie or possible winner");
        } else {
          require(visits[row + 1] ==
                      unsigned(selected[row] || fixture.expected[row] <= initial),
                  "finite mask differs from independent integer oracle");
        }
      }
    };
    run(std::numeric_limits<uint32_t>::max(), false);
    if (!eligible) continue;
    const uint32_t middle = fixture.expected[fixture.rows / 2];
    for (uint32_t boundary : {uint32_t{0}, fixture.expected.front(),
                             middle == 0 ? 0 : middle - 1, middle})
      run(boundary, false);
    run(std::numeric_limits<uint32_t>::max(), true);
    run(std::numeric_limits<uint32_t>::max(), true, &reused);
    run(middle, false, &reused);
    const size_t reused_failures = allocation_failures;
    fail_bytes = (fixture.leading + fixture.rows + 63) / 64 * 64 * sizeof(uint32_t);
    try { run(std::numeric_limits<uint32_t>::max(), true, &reused); }
    catch (...) { fail_bytes = 0; throw; }
    fail_bytes = 0;
    require(allocation_failures == reused_failures,
            "warm workspace unexpectedly allocated score storage");

    // All storage used by these callbacks is already allocated. Fail the
    // extent-sized scratch allocation and require a side-effect-free fallback.
    reset(std::numeric_limits<uint32_t>::max());
    const size_t failures_before = allocation_failures;
    fail_bytes = (fixture.leading + fixture.rows + 63) / 64 * 64 * sizeof(uint32_t);
    bool accepted = false;
    try {
      accepted = visit(fixture.codes(), fixture.query.data() + 1,
          fixture.dimension, fixture.row_norms(), fixture.correction,
          fixture.rows, fixture.first_row, capacity,
          [&](size_t index) { consume(index, true, false); },
          [&](size_t index) { consume(index, false, false); },
          [&] { ++boundaries; return above; }, fixture.leading);
    } catch (...) {
      fail_bytes = 0;
      throw;
    }
    fail_bytes = 0;
    require(allocation_failures == failures_before + 1,
            "extent-sized allocation failure was not exercised");
    require(!accepted && seeds == 0 && survivors == 0 && boundaries == 0 &&
                std::all_of(visits.begin() + 1, visits.end() - 1,
                            [](uint32_t count) { return count == 0; }),
            "allocation failure consumed rows or queried the boundary");
  }
  require(fixture.packed == packed_before && fixture.norms == norms_before &&
              fixture.query == query_before,
          "shared matrix, padding or input sentinel overwritten");
}

int main() {
  using namespace mvd::quantized_seed;
  Scratch workspace, separate;
  require(workspace.prepare(512) && separate.prepare(512) &&
              workspace.data() != separate.data(), "range scratch is shared");
  auto *saved = workspace.data();
  saved[0] = 12345;
  fail_bytes = 1024 * sizeof(uint32_t);
  const bool grew = workspace.prepare(1024);
  fail_bytes = 0;
  require(!grew && workspace.data() == saved && saved[0] == 12345 &&
              workspace.prepare(256), "failed growth discarded existing scratch");
  require(!workspace.prepare(65601) && workspace.data() == saved,
          "scratch exceeded its fixed storage bound");
  require(scan_kernel(63) == nullptr && scan_kernel(65) == nullptr,
          "unsupported dimension admitted");
  if (scan_kernel(64) == nullptr) {
    std::cout << "SKIP: AVX512VNNI unavailable\n";
    return 0;
  }
  std::mt19937 random(20261117);
  constexpr uint32_t guard = 0xdeadbeef;
  for (size_t dimension : {64, 128, 256}) {
    for (size_t rows : {64, 192, 2240, 9984}) {
      for (int pattern : {0, 1, 2}) {
        std::vector<int8_t> vectors(rows * dimension), query(dimension + 1);
        std::vector<uint8_t> packed(rows * dimension + 2, 77);
        std::vector<uint32_t> norms(rows + 2, guard), scores(rows + 2, guard);
        std::array<uint32_t, 66> seed_scores, seed_rows;
        seed_scores.fill(guard); seed_rows.fill(guard);
        int64_t correction = 0;
        for (size_t axis = 0; axis < dimension; ++axis) {
          const int q = pattern == 0 ? static_cast<int>(random() % 256) - 128
              : pattern == 1 ? 127 : -128;
          query[axis + 1] = static_cast<int8_t>(q);
          correction += int64_t(q + 128) * (q + 128) - 128 * 128;
        }
        std::vector<uint32_t> expected(rows);
        for (size_t row = 0; row < rows; ++row) {
          uint32_t norm = 0, distance = 0;
          for (size_t axis = 0; axis < dimension; ++axis) {
            const int value = pattern == 0 ? static_cast<int>(random() % 255) - 127
                : pattern == 1 ? -127 : 127;
            vectors[row * dimension + axis] = static_cast<int8_t>(value);
            norm += value * value;
            const int difference = value - query[axis + 1];
            distance += difference * difference;
            packed[1 + (row / 16) * 16 * dimension + (axis / 4) * 64 +
                   (row % 16) * 4 + axis % 4] = static_cast<uint8_t>(value + 128);
          }
          norms[row + 1] = norm;
          expected[row] = distance;
        }
        scan_kernel(dimension)(packed.data() + 1, query.data() + 1, dimension,
            norms.data() + 1, correction, rows, scores.data() + 1,
            seed_scores.data() + 1, seed_rows.data() + 1);
        require(std::equal(expected.begin(), expected.end(), scores.begin() + 1),
                "code scores disagree with independent squared differences");
        for (size_t lane = 0; lane < 64; ++lane) {
          size_t best = lane;
          for (size_t row = lane + 64; row < rows; row += 64) {
            if (expected[row] < expected[best]) best = row;
          }
          require(seed_rows[lane + 1] == best &&
                      seed_scores[lane + 1] == expected[best],
                  "seed minimum, index or first-tie selection differs");
        }
        require(scores.front() == guard && scores.back() == guard &&
                    norms.front() == guard && norms.back() == guard &&
                    seed_rows.front() == guard && seed_rows.back() == guard &&
                    seed_scores.front() == guard && seed_scores.back() == guard &&
                    packed.front() == 77 && packed.back() == 77,
                "buffer guard overwritten");
        for (size_t first = 0; first < rows; first += 64) {
          for (uint32_t boundary : {uint32_t{0}, expected[first],
                   uint32_t{0x7fffffff}, uint32_t{0x80000000},
                   std::numeric_limits<uint32_t>::max()}) {
            uint64_t mask = 0;
            for (size_t lane = 0; lane < 64; ++lane)
              mask |= uint64_t(expected[first + lane] <= boundary) << lane;
            require(mask_kernel()(scores.data() + first + 1, boundary) == mask,
                    "saved-score mask differs at unsigned boundary");
          }
        }
        if (rows >= 2112) {
          // Start within a shared matrix and finish partway through a block.
          // With an infinite boundary every real row must be visited once,
          // independently of which quantized minima were used as seeds.
          for (size_t length : {2049, 2063, 2064, 2111}) {
            constexpr size_t first = 16;
            for (size_t capacity : {size_t{1}, size_t{10}, size_t{32}}) {
              std::vector<unsigned> visits(length);
              const auto consume = [&](size_t index) {
                require(index >= first && index < first + length, "visited outside bucket");
                ++visits[index - first];
              };
              require(visit(packed.data() + 1 + first * dimension, query.data() + 1,
                  dimension, norms.data() + 1 + first, correction, length,
                  first, capacity, consume, consume,
                  [] { return std::numeric_limits<uint32_t>::max(); }),
                  "eligible visit fell back");
              require(std::all_of(visits.begin(), visits.end(),
                                 [](unsigned count) { return count == 1; }),
                      "seed duplicated or unseeded/tail row lost");
              fail_bytes = (length + 63) / 64 * 64 * sizeof(uint32_t);
              std::fill(visits.begin(), visits.end(), 0);
              const bool accepted = visit(packed.data() + 1 + first * dimension,
                  query.data() + 1, dimension, norms.data() + 1 + first,
                  correction, length, first, capacity, consume, consume,
                  [] { return std::numeric_limits<uint32_t>::max(); });
              fail_bytes = 0;
              require(!accepted && std::all_of(visits.begin(), visits.end(),
                                     [](unsigned count) { return count == 0; }),
                      "allocation failure partially consumed the range");
            }
          }
        }
      }
    }
  }
  // Keep the physical block offset independent of the callback index origin.
  // In particular code_first=16 must not be mistaken for leading_rows=16.
  constexpr size_t code_offsets[] = {0, 16, 48};
  constexpr size_t index_origins[] = {0, 16, 1009};
  for (size_t dimension : {64, 128, 256}) {
    for (size_t leading = 0; leading < 16; ++leading) {
      for (size_t rows : {511, 512, 513, 1023, 2049, 2500}) {
        for (int pattern : {0, 1, 2}) {
          check_bucket_visit(BucketFixture(dimension, rows, leading,
              code_offsets[(leading + pattern) % 3],
              index_origins[(rows + pattern) % 3], pattern));
        }
      }
    }
    for (size_t leading : {size_t{0}, size_t{1}, size_t{15}})
      for (size_t rows : {65535, 65536, 65537})
        check_bucket_visit(BucketFixture(dimension, rows, leading, 16, 1009, 1));
  }
  const BucketFixture rejected(64, 512, 15, 16, 1009, 0);
  size_t callbacks = 0;
  const auto consume = [&](size_t) { ++callbacks; };
  const auto boundary = [&] { ++callbacks; return uint32_t{0}; };
  for (size_t capacity : {size_t{0}, size_t{33}})
    require(!visit(rejected.codes(), rejected.query.data() + 1, 64,
        rejected.row_norms(), rejected.correction, rejected.rows,
        rejected.first_row, capacity, consume, consume, boundary, rejected.leading),
        "unsupported capacity admitted");
  for (size_t dimension : {size_t{63}, size_t{65}})
    require(!visit(rejected.codes(), rejected.query.data() + 1, dimension,
        rejected.row_norms(), rejected.correction, rejected.rows,
        rejected.first_row, 10, consume, consume, boundary, rejected.leading),
        "unsupported visit dimension admitted");
  for (size_t rows : {size_t{0}, size_t{1}})
    require(!visit(rejected.codes(), rejected.query.data() + 1, 64,
        rejected.row_norms(), rejected.correction, rows,
        rejected.first_row, 10, consume, consume, boundary, rejected.leading),
        "empty or undersized bucket admitted");
  require(callbacks == 0, "rejected visit invoked a callback");
  std::cout << "PASS: seed scores, indices, ties, unaligned buffers and masks\n";
}
