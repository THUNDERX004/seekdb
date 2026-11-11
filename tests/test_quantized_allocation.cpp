#include <cstdlib>
#include <new>

namespace {
long long allocation_budget = -1;
std::size_t allocation_calls = 0;
bool allocation_injected = false;
}

void *operator new(std::size_t size) {
  ++allocation_calls;
  if (allocation_budget == 0) {
    allocation_budget = -1;
    allocation_injected = true;
    throw std::bad_alloc();
  }
  if (allocation_budget > 0) {
    --allocation_budget;
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

// Include the real implementation; only this standalone test replaces new.
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
constexpr std::size_t row_count = 257;
constexpr std::size_t dimension = 17;

mvd::TableDef fixtureDefinition() {
  mvd::TableDef definition;
  definition.columns = {{"id", mvd::ColumnType::BigInt, 0},
                        {"v", mvd::ColumnType::Vector, dimension}};
  definition.vector_column = 1;
  return definition;
}

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct Fixture {
  std::array<std::array<float, dimension>, row_count> vectors{};
  mvd::Table::IndexedRows rows;
  const mvd::Table::IndexedRow *directory = nullptr;

  Fixture() {
    rows.reserve(row_count);
    for (std::size_t i = 0; i < row_count; ++i) {
      vectors[i].fill(static_cast<float>(i % 7 + 1));
      rows.push_back({static_cast<int64_t>(i), static_cast<uint64_t>(i),
                      nullptr, vectors[i].data()});
    }
    directory = rows.data();
  }

  void checkDirectory() const {
    require(rows.size() == row_count && rows.data() == directory,
            "quantization invalidated the borrowed directory");
    for (std::size_t i = 0; i < row_count; ++i) {
      const auto &row = directory[i];
      require(row.primary == static_cast<int64_t>(i) &&
                  row.offset == static_cast<uint64_t>(i) &&
                  row.cached == nullptr && row.vector_data == vectors[i].data(),
              "quantization changed a borrowed directory entry");
      for (std::size_t axis = 0; axis < dimension; ++axis) {
        require(row.vector_data[axis] == static_cast<float>(i % 7 + 1),
                "borrowed vector contents changed");
      }
    }
  }
};

bool prepare(mvd::Table &table, const mvd::Table::IndexedRows &rows,
             long long budget, bool blocked) {
  allocation_calls = 0;
  allocation_injected = false;
  allocation_budget = budget;
  try {
    const bool ready = table.prepareQuantizedRows(rows, dimension, blocked);
    allocation_budget = -1;
    return ready;
  } catch (...) {
    allocation_budget = -1;
    throw;
  }
}

void checkUnpublished(const mvd::Table::IndexedRows &rows) {
  require(rows.codes_attempted, "failed preparation was not remembered");
  require(rows.codes.empty() && rows.codes.capacity() == 0 &&
              rows.code_norms.empty() && rows.code_norms.capacity() == 0 &&
              rows.code_step == 0 && rows.code_error == 0 &&
              rows.code_stride == 0 && !rows.codes_blocked &&
              rows.code_source == nullptr && rows.code_first == 0,
          "failed preparation published a partial quantized cache");
}

void checkResidualBound(const mvd::Table::IndexedRows &rows) {
  require(std::isfinite(rows.code_error) && rows.code_error >= 0,
          "quantized residual bound is not finite and nonnegative");
  double maximum = 0;
  for (size_t row = 0; row < rows.size(); ++row) {
    double residual = 0;
    uint32_t norm = 0;
    for (size_t axis = 0; axis < dimension; ++axis) {
      int code;
      if (rows.codes_blocked) {
        // Decode the documented four-axis groups, including partial blocks;
        // this check never uses the production residual sum or its envelope.
        const auto *block = reinterpret_cast<const uint8_t *>(rows.codes.data()) +
            (row / 16) * 16 * rows.code_stride;
        code = static_cast<int>(block[(axis / 4 * 16 + row % 16) * 4 + axis % 4]) - 128;
      } else {
        code = rows.codes[row * rows.code_stride + axis];
      }
      const double scaled = static_cast<double>(rows[row].vector_data[axis]) / rows.code_step;
      norm += static_cast<uint32_t>(code * code);
      residual = std::hypot(residual, scaled - code);
    }
    require(std::isfinite(residual), "independent residual norm is not finite");
    maximum = std::max(maximum, residual);
    require(rows.code_norms[row] == norm,
            "quantized norm does not match its independently decoded row");
  }
  require(rows.code_error >= maximum,
          "quantized residual bound understates an independent double norm");
}

void checkFailure(long long budget, bool blocked) {
  mvd::Table table(fixtureDefinition(), "unused-quantized-allocation.dat");
  table.setCacheLimit(1024 * 1024);
  Fixture fixture;

  // Budget 0 fails codes; budget 1 allows codes and then fails norms.
  require(!prepare(table, fixture.rows, budget, blocked),
          "allocation failure did not return the float-path fallback");
  require(allocation_injected &&
              allocation_calls == static_cast<std::size_t>(budget + 1),
          "did not inject the requested optional allocation failure");
  checkUnpublished(fixture.rows);
  fixture.checkDirectory();

  require(!prepare(table, fixture.rows, 0, blocked),
          "failed cache unexpectedly became available on retry");
  require(!allocation_injected && allocation_calls == 0,
          "failed cache retried an allocation");
  checkUnpublished(fixture.rows);
  fixture.checkDirectory();
  std::cout << "PASS quantized allocation failure budget=" << budget
            << " blocked=" << blocked << '\n';
}

void checkSuccess(bool blocked) {
  mvd::Table table(fixtureDefinition(), "unused-quantized-allocation.dat");
  table.setCacheLimit(1024 * 1024);
  Fixture fixture;
  require(prepare(table, fixture.rows, -1, blocked), "fresh quantized cache did not build");
  const size_t stride = blocked ? 20 : dimension;
  const size_t stored_rows = blocked ? 272 : row_count;
  require(fixture.rows.codes_attempted &&
              fixture.rows.codes.size() == stored_rows * stride &&
              fixture.rows.code_norms.size() == stored_rows &&
              fixture.rows.code_stride == stride && fixture.rows.codes_blocked == blocked &&
              fixture.rows.code_step > 0 && std::isfinite(fixture.rows.code_step) &&
              fixture.rows.code_source == nullptr && fixture.rows.code_first == 0,
          "successful preparation did not publish a complete cache");
  checkResidualBound(fixture.rows);
  fixture.checkDirectory();

  const auto *codes = fixture.rows.codes.data();
  const auto *norms = fixture.rows.code_norms.data();
  const double error = fixture.rows.code_error;
  require(prepare(table, fixture.rows, 0, blocked), "published cache could not be reused");
  require(!allocation_injected && allocation_calls == 0 &&
              fixture.rows.codes.data() == codes &&
              fixture.rows.code_norms.data() == norms && fixture.rows.code_error == error,
          "published cache was reallocated on reuse");
  fixture.checkDirectory();
  if (blocked) {
    std::array<int8_t, 20> query{};
    for (size_t axis = 0; axis < dimension; ++axis) {
      query[axis] = static_cast<int8_t>(int(axis % 5) * 31 - 60);
    }
    const auto correction = mvd::quantized_block::query_correction(query.data(), dimension);
    for (size_t first = 0; first < stored_rows; first += 16) {
      uint32_t scores[16];
      mvd::quantized_block::scalar(
          reinterpret_cast<const uint8_t *>(codes + first * stride), query.data(),
          stride, norms + first, correction, scores);
      for (size_t row = 0; row < 16; ++row) {
        // This fixture's scale is exactly 1/16, so the independent expected
        // code is an integer multiple of 16. Padded rows represent zero.
        const int code = first + row < row_count ? int((first + row) % 7 + 1) * 16 : 0;
        uint32_t expected = 0;
        for (size_t axis = 0; axis < dimension; ++axis) {
          const int delta = code - query[axis];
          expected += static_cast<uint32_t>(delta * delta);
        }
        require(scores[row] == expected, "packed row/axis tails changed a squared score");
      }
    }
  }
  mvd::Table::IndexedRows empty;
  fixture.rows.swap(empty);
  require(fixture.rows.codes.empty() && fixture.rows.code_norms.empty() &&
              fixture.rows.code_step == 0 && fixture.rows.code_error == 0 &&
              fixture.rows.code_stride == 0 && !fixture.rows.codes_blocked &&
              !fixture.rows.codes_attempted && fixture.rows.code_source == nullptr &&
              fixture.rows.code_first == 0, "cache invalidation retained layout metadata");
  require(empty.code_error == error && empty.codes.data() == codes &&
              empty.code_norms.data() == norms,
          "swap lost published quantization storage or its residual bound");
  // Exercise nondefault borrowing metadata separately from the owning matrix.
  fixture.rows.code_source = &empty;
  fixture.rows.code_first = 17;
  mvd::Table::IndexedRows borrowed;
  fixture.rows.swap(borrowed);
  require(fixture.rows.code_source == nullptr && fixture.rows.code_first == 0 &&
              fixture.rows.code_error == 0 && borrowed.code_source == &empty &&
              borrowed.code_first == 17,
          "swap failed to transfer or reset shared matrix metadata");
  std::cout << "PASS quantized cache construction, packing and reuse blocked=" << blocked << '\n';
}

void checkResidualFixtures(bool blocked) {
  // Three small fixtures cover ordinary rounding/clamping, the largest finite
  // scale mixed with subnormals, and a wholly subnormal quantization scale.
  constexpr std::array<std::array<uint32_t, 8>, 3> patterns{{
      {{0x3EFFFFFF, 0x3F000000, 0x3F000001, 0x42FEFFFF,
        0x42FF0000, 0x42FF0001, 0x42FFFFFF, 0}},
      {{0x7F7FFFFF, 0x7F000001, 0x7EFFFFFF, 0x3F800001,
        1, 0x007FFFFF, 0x00800000, 0}},
      {{1, 2, 3, 0x00008001, 0x003FFFFF, 0x00400001, 0x007FFFFF, 0}}
  }};
  for (const auto &pattern : patterns) {
    mvd::Table table(fixtureDefinition(), "unused-quantized-allocation.dat");
    table.setCacheLimit(1024 * 1024);
    Fixture fixture;
    for (size_t row = 0; row < row_count; ++row) {
      for (size_t axis = 0; axis < dimension; ++axis) {
        uint32_t bits = pattern[(row * 3 + axis * 5) % pattern.size()];
        if ((row + axis) % 2) bits |= 0x80000000U;
        fixture.vectors[row][axis] = std::bit_cast<float>(bits);
      }
    }
    require(prepare(table, fixture.rows, -1, blocked),
            "finite boundary fixture did not build a quantized cache");
    require(std::isfinite(fixture.rows.code_step) && fixture.rows.code_step > 0,
            "finite boundary fixture published an invalid scale");
    checkResidualBound(fixture.rows);
    require(fixture.rows.code_error > 0.5,
            "boundary fixture did not exercise a nonzero residual norm");
  }
  std::cout << "PASS independent quantized residual bounds blocked=" << blocked << '\n';
}

void checkPaddingBudget() {
  mvd::Table table(fixtureDefinition(), "unused-quantized-allocation.dat");
  table.setCacheLimit(6000);  // 5397 row-major bytes fit; 6528 padded bytes do not.
  Fixture fixture;
  require(!prepare(table, fixture.rows, -1, true), "padding exceeded the cache budget");
  require(allocation_calls == 0, "padding budget was checked after allocation");
  checkUnpublished(fixture.rows);
  fixture.checkDirectory();
  std::cout << "PASS padded allocation budget\n";
}

void checkSharedDirectoryFailure(bool blocked) {
  constexpr size_t total = 2049;
  mvd::TableDef definition;
  definition.oid = 1;
  definition.columns = {{"id", mvd::ColumnType::BigInt, 0},
                        {"tag", mvd::ColumnType::BigInt, 0},
                        {"v", mvd::ColumnType::Vector, dimension}};
  definition.vector_column = 2;
  mvd::Table table(std::move(definition), "unused-shared-quantized-allocation.dat");
  table.setCacheLimit(8 * 1024 * 1024);
  {
    std::vector<mvd::Row> values(total, mvd::Row(3));
    std::vector<mvd::PendingRecord> pending;
    pending.reserve(total);
    for (size_t i = 0; i < total; ++i) {
      values[i][0].setInteger(static_cast<int64_t>(i));
      values[i][1].setInteger(i < row_count ? 7 : 8);
      values[i][2].setVector(std::vector<float>(dimension, float(i % 7 + 1)));
      pending.push_back({1, static_cast<int64_t>(i), 1, &values[i], i});
    }
    table.applyPending(pending);
  }
  require(table.fullyCached() && table.rowCount() == total,
          "shared-directory fixture is not fully cached");
  mvd::Condition condition;
  condition.column = 1;
  condition.op = mvd::CompareOp::Eq;
  condition.value.setInteger(7);
  const auto *bucket = table.cachedDistanceCandidates(2, &condition);
  require(bucket != nullptr && bucket->size() == row_count &&
              bucket->code_source == nullptr,
          "shared-directory fixture did not create an independent bucket");
  const auto *directory = bucket->data();
  const auto saved = bucket->rows;
  const auto checkBorrowed = [&] {
    require(bucket->data() == directory && bucket->size() == saved.size(),
            "shared-directory preparation replaced the borrowed bucket");
    for (size_t i = 0; i < saved.size(); ++i) {
      const auto &row = (*bucket)[i];
      require(row.primary == saved[i].primary && row.offset == saved[i].offset &&
                  row.cached == saved[i].cached && row.vector_data == saved[i].vector_data,
              "shared-directory preparation changed a borrowed row");
    }
  };
  const auto quantize = [&](long long budget) {
    allocation_calls = 0;
    allocation_injected = false;
    allocation_budget = budget;
    try {
      const auto *result = table.quantizedDistanceRows(*bucket, 2, dimension, blocked);
      allocation_budget = -1;
      return result;
    } catch (...) {
      allocation_budget = -1;
      throw;
    }
  };
  // The equality directory is already warm; the first allocation is the full
  // directory's reserve, before any shared bindings or codes are published.
  require(quantize(0) == nullptr && allocation_injected && allocation_calls == 1,
          "full-directory reserve failure did not return the exact-scan fallback");
  require(!bucket->codes_attempted && bucket->codes.empty() &&
              bucket->code_norms.empty() && bucket->code_source == nullptr &&
              bucket->code_first == 0,
          "failed shared-directory reserve published partial metadata");
  checkBorrowed();
  require(table.cachedDistanceCandidates(2, &condition) == bucket,
          "failed shared-directory reserve discarded the equality bucket");

  const auto *full = quantize(-1);
  require(full != nullptr && full != bucket && full->size() == total &&
              bucket->code_source == full && bucket->code_first <= total - row_count,
          "shared-directory construction did not recover after reserve failure");
  checkBorrowed();
  for (size_t i = 0; i < saved.size(); ++i) {
    const auto &row = (*full)[bucket->code_first + i];
    require(row.primary == saved[i].primary && row.cached == saved[i].cached &&
                row.vector_data == saved[i].vector_data,
            "recovered shared slice does not match the borrowed bucket");
  }
  const auto *codes = full->codes.data();
  const auto *norms = full->code_norms.data();
  require(quantize(0) == full && !allocation_injected && allocation_calls == 0 &&
              full->codes.data() == codes && full->code_norms.data() == norms,
          "recovered shared directory could not be reused without allocation");
  checkBorrowed();
  require(table.cachedDistanceCandidates(2, nullptr) == full,
          "subsequent full query did not reuse the recovered directory");
  std::vector<std::pair<double, int64_t>> actual, expected;
  for (size_t i = 0; i < total; ++i) {
    const double value = static_cast<double>(i % 7 + 1);
    expected.emplace_back(dimension * value * value, static_cast<int64_t>(i));
  }
  table.forEachIndexedBase(*full, nullptr,
      [&](int64_t primary, mvd::RowView row, bool stable) {
    require(stable && row[0].integer() == primary && row[2].vector().size() == dimension,
            "recovered full query lost a cached row");
    double squared = 0;
    for (float value : row[2].vector()) { squared += double(value) * value; }
    actual.emplace_back(squared, primary);
    return true;
  });
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  require(actual == expected, "recovered full query changed exact distance results");
  std::cout << "PASS shared-directory reserve failure, recovery and reuse blocked="
            << blocked << '\n';
}
void checkPreservedFullDirectory(bool blocked) {
  struct PreviousVersion { uint64_t offset; bool cached; mvd::Row row; };
#ifdef MVD_OWNING_ROW_CACHE
  static_assert(sizeof(void *) != 8 || sizeof(mvd::Version) == sizeof(PreviousVersion));
#else
  static_assert(sizeof(void *) != 8 || sizeof(mvd::Version) == 24);
#endif
  constexpr size_t total = 2049;
  mvd::TableDef definition;
  definition.oid = 1;
  definition.columns = {{"id", mvd::ColumnType::BigInt, 0},
                        {"tag", mvd::ColumnType::BigInt, 0},
                        {"category", mvd::ColumnType::BigInt, 0},
                        {"v", mvd::ColumnType::Vector, dimension}};
  definition.vector_column = 3;
  mvd::Table table(std::move(definition), "unused-retained-quantized-cache.dat");
  table.setCacheLimit(8 * 1024 * 1024);
  size_t row_bytes = 0;
  {
    std::vector<mvd::Row> values(total, mvd::Row(4));
    std::vector<mvd::PendingRecord> pending;
    pending.reserve(total);
    for (size_t i = 0; i < total; ++i) {
      values[i][0].setInteger(static_cast<int64_t>(i));
      values[i][1].setInteger(i < row_count ? 7 : 8);
      values[i][2].setInteger(static_cast<int64_t>(i % 3));
      values[i][3].setVector(std::vector<float>(dimension, float(i % 7 + 1)));
      row_bytes += mvd::cachedRowBytes(values[i]);
      pending.push_back({1, static_cast<int64_t>(i), 1, &values[i], i});
    }
    table.applyPending(pending);
  }
  const auto *full = table.cachedDistanceCandidates(3, nullptr);
  require(full != nullptr && table.prepareQuantizedRows(*full, dimension, blocked),
          "full-first fixture did not prepare its screening cache");
  row_bytes = 0;
  for (const auto &entry : *full) row_bytes += mvd::cachedRowBytes(*entry.cached);
  const auto *directory = full->data();
  const auto *codes = full->codes.data();
  const auto *norms = full->code_norms.data();
  const auto verifyPreserved = [&] {
    allocation_calls = 0;
    allocation_injected = false;
    allocation_budget = 0;
    const auto *again = table.cachedDistanceCandidates(3, nullptr);
    const bool ready = again != nullptr &&
        table.prepareQuantizedRows(*again, dimension, blocked);
    allocation_budget = -1;
    require(ready && again == full && again->data() == directory &&
                again->codes.data() == codes && again->code_norms.data() == norms &&
                allocation_calls == 0 && !allocation_injected,
            "predicate switch discarded or rebuilt an unchanged full matrix");
    checkResidualBound(*again);
  };
  mvd::Condition condition;
  condition.column = 1;
  condition.op = mvd::CompareOp::Eq;
  condition.value.setInteger(7);
  allocation_calls = 0;
  const auto *bucket = table.cachedDistanceCandidates(3, &condition);
  const size_t first_build_allocations = allocation_calls;
  require(bucket != nullptr && bucket->size() == row_count &&
              bucket->code_source == full &&
              bucket->code_first <= total - row_count,
          "new equality bucket did not reuse the permuted full matrix");
  for (size_t i = 0; i < bucket->size(); ++i) {
    require((*full)[bucket->code_first + i].primary == (*bucket)[i].primary,
            "permuted full-matrix range does not match its equality bucket");
  }
  verifyPreserved();

  // Failure after dropping the old predicate must not drop the full cache or
  // publish a partial new equality index. A subsequent request can rebuild it.
  condition.column = 2;
  condition.value.setInteger(1);
  allocation_calls = 0;
  allocation_injected = false;
  allocation_budget = 0;
  bool failed = false;
  try {
    (void)table.cachedDistanceCandidates(3, &condition);
  } catch (const std::bad_alloc &) {
    failed = true;
  }
  allocation_budget = -1;
  require(failed && allocation_injected, "predicate failure was not injected");
  verifyPreserved();
  bucket = table.cachedDistanceCandidates(3, &condition);
  require(bucket != nullptr && bucket->size() == total / 3,
          "predicate switch did not recover a complete equality directory");
  for (const auto &row : *bucket) {
    require(row.cached != nullptr && table.view(row.primary, *row.cached)[2].integer() == 1,
            "recovered predicate returned a row from another bucket");
  }
  verifyPreserved();

  // The last allocation when rebuilding the original predicate is its small
  // permutation. Failure is optional: the complete index still works, codes
  // stay in their original order, and a later alignment may safely recover.
  condition.column = 1;
  condition.value.setInteger(7);
  const auto saved_codes = full->codes;
  const auto saved_norms = full->code_norms;
  const auto saved_rows = full->rows;
  allocation_calls = 0;
  allocation_injected = false;
  allocation_budget = static_cast<long long>(first_build_allocations) - 1;
  bucket = table.cachedDistanceCandidates(3, &condition);
  allocation_budget = -1;
  require(allocation_injected && bucket != nullptr && bucket->size() == row_count &&
              bucket->code_source == nullptr && full->codes == saved_codes &&
              full->code_norms == saved_norms,
          "optional permutation failure changed codes or dropped the equality index");
  for (size_t i = 0; i < saved_rows.size(); ++i) {
    require((*full)[i].primary == saved_rows[i].primary,
            "failed permutation partially reordered the full directory");
  }
  verifyPreserved();
  table.alignCachedDistanceRows();
  require(bucket->code_source == full, "optional permutation did not recover");
  verifyPreserved();

  // Reserve enough for the rows, full directory and matrix, but not another
  // complete index. Retained storage must be included in the shared budget.
  const size_t full_bytes = full->capacity() * sizeof(mvd::Table::IndexedRow) +
      full->codes.capacity() + full->code_norms.capacity() * sizeof(uint32_t);
  table.setCacheLimit(row_bytes + full_bytes + 512);
  full = table.cachedDistanceCandidates(3, nullptr);
  require(full != nullptr && table.prepareQuantizedRows(*full, dimension, blocked),
          "tight-budget fixture did not fit the full matrix");
  directory = full->data();
  codes = full->codes.data();
  norms = full->code_norms.data();
  require(table.cachedDistanceCandidates(3, &condition) == nullptr,
          "equality construction ignored retained full-cache bytes");
  verifyPreserved();

  mvd::Row replacement(4);
  replacement[0].setInteger(0);
  replacement[1].setInteger(7);
  replacement[2].setInteger(1);
  replacement[3].setVector(std::vector<float>(dimension, 99.0f));
  table.applyPending({{1, 0, 2, &replacement, 99999}});
  full = table.cachedDistanceCandidates(3, nullptr);
  require(full != nullptr && full->codes.empty() && !full->codes_attempted,
          "committed replacement retained stale screening codes");
  bool found = false;
  for (const auto &row : *full) {
    if (row.primary == 0) {
      found = true;
      require(row.cached != nullptr && row.vector_data[0] == 99.0f &&
                  table.view(row.primary, *row.cached)[2].integer() == 1,
              "rebuilt directory retained a replaced row address");
    }
  }
  require(found && table.prepareQuantizedRows(*full, dimension, blocked),
          "committed replacement did not rebuild a complete full matrix");
  checkResidualBound(*full);
  table.setCacheLimit(0);
  require(!table.fullyCached() && table.cachedDistanceCandidates(3, nullptr) == nullptr,
          "cache eviction retained row addresses or screening codes");
  std::cout << "PASS full-first predicate reuse, failure, budget and commit blocked="
            << blocked << '\n';
}
}  // namespace

int main() {
  try {
    for (bool blocked : {false, true}) {
      checkFailure(0, blocked);
      checkFailure(1, blocked);
      checkSuccess(blocked);
      checkResidualFixtures(blocked);
      checkSharedDirectoryFailure(blocked);
      checkPreservedFullDirectory(blocked);
    }
    checkPaddingBudget();
    return 0;
  } catch (const std::exception &error) {
    allocation_budget = -1;
    std::cerr << "FAIL quantized allocation regression: " << error.what() << '\n';
    return 1;
  }
}
