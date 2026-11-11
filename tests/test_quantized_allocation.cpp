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
             long long budget) {
  allocation_calls = 0;
  allocation_injected = false;
  allocation_budget = budget;
  try {
    const bool ready = table.prepareQuantizedRows(rows, dimension);
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
              rows.code_step == 0,
          "failed preparation published a partial quantized cache");
}

void checkFailure(long long budget) {
  mvd::Table table(mvd::TableDef{}, "unused-quantized-allocation.dat");
  table.setCacheLimit(1024 * 1024);
  Fixture fixture;

  // Budget 0 fails codes; budget 1 allows codes and then fails norms.
  require(!prepare(table, fixture.rows, budget),
          "allocation failure did not return the float-path fallback");
  require(allocation_injected &&
              allocation_calls == static_cast<std::size_t>(budget + 1),
          "did not inject the requested optional allocation failure");
  checkUnpublished(fixture.rows);
  fixture.checkDirectory();

  require(!prepare(table, fixture.rows, 0),
          "failed cache unexpectedly became available on retry");
  require(!allocation_injected && allocation_calls == 0,
          "failed cache retried an allocation");
  checkUnpublished(fixture.rows);
  fixture.checkDirectory();
  std::cout << "PASS quantized allocation failure budget=" << budget << '\n';
}

void checkSuccess() {
  mvd::Table table(mvd::TableDef{}, "unused-quantized-allocation.dat");
  table.setCacheLimit(1024 * 1024);
  Fixture fixture;
  require(prepare(table, fixture.rows, -1), "fresh quantized cache did not build");
  require(fixture.rows.codes_attempted &&
              fixture.rows.codes.size() == row_count * dimension &&
              fixture.rows.code_norms.size() == row_count &&
              fixture.rows.code_step > 0 && std::isfinite(fixture.rows.code_step),
          "successful preparation did not publish a complete cache");
  fixture.checkDirectory();

  const auto *codes = fixture.rows.codes.data();
  const auto *norms = fixture.rows.code_norms.data();
  require(prepare(table, fixture.rows, 0), "published cache could not be reused");
  require(!allocation_injected && allocation_calls == 0 &&
              fixture.rows.codes.data() == codes &&
              fixture.rows.code_norms.data() == norms,
          "published cache was reallocated on reuse");
  fixture.checkDirectory();
  std::cout << "PASS quantized cache construction and reuse\n";
}
}  // namespace

int main() {
  try {
    checkFailure(0);
    checkFailure(1);
    checkSuccess();
    return 0;
  } catch (const std::exception &error) {
    allocation_budget = -1;
    std::cerr << "FAIL quantized allocation regression: " << error.what() << '\n';
    return 1;
  }
}
