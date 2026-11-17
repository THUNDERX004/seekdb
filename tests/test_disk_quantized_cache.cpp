#include <chrono>
#include <cstdlib>
#include <new>

namespace {
long long allocation_budget = -1;
bool allocation_injected = false;
}
void *operator new(std::size_t size) {
  if (allocation_budget == 0) {
    allocation_budget = -1;
    allocation_injected = true;
    throw std::bad_alloc();
  }
  if (allocation_budget > 0) --allocation_budget;
  if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

#define MVD_TEST_PHYSICAL_ORDER
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main
#undef MVD_TEST_PHYSICAL_ORDER

namespace mvd {
struct PhysicalOrderTest {
  static size_t retained(const Table &table) {
    return table.cache_bytes_ + table.equality_bytes_ + table.quantized_bytes_ +
        table.cached_distance_rows_.capacity() * sizeof(Table::IndexedRow) +
        table.disk_distance_bytes_;
  }
  static size_t disk(const Table &table) { return table.disk_distance_bytes_; }
  static void clear(Table &table) { table.clearDiskDistanceCodes(); }
};
}

namespace {
using Access = mvd::PhysicalOrderTest;
constexpr size_t count = 513;
constexpr size_t budget = 120 * 1024;
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
mvd::TableDef definition() {
  mvd::TableDef result;
  result.oid = 1;
  result.name = "disk";
  result.columns = {{"id", mvd::ColumnType::BigInt, 0},
                    {"tag", mvd::ColumnType::BigInt, 0},
                    {"v", mvd::ColumnType::Vector, 128},
                    {"w", mvd::ColumnType::Vector, 65}, {"category", mvd::ColumnType::VarChar, 16}};
  result.vector_column = 2;
  return result;
}
mvd::Row row(int64_t primary, int revision = 0) {
  mvd::Row result(5);
  result[0].setInteger(primary);
  result[1].setInteger(primary % 4);
  for (size_t column = 2; column <= 3; ++column) {
    const size_t dimension = column == 2 ? 128 : 65;
    std::vector<float> vector(dimension);
    for (size_t axis = 0; axis < dimension; ++axis) {
      vector[axis] = static_cast<float>((primary * 7 + axis + revision) % 53) / 8 - 3;
    }
    result[column].setVector(std::move(vector));
  }
  result[4].setText(primary % 3 == 0 ? "red" : "blue");
  return result;
}
void commit(mvd::Table &table, const mvd::fs::path &directory, uint64_t txid,
            std::map<int64_t, std::optional<mvd::Row>> changes) {
  std::vector<mvd::PendingRecord> pending;
  {
    mvd::FileWriter writer(table.dataPath());
    pending = table.appendChanges(txid, writer, changes);
    writer.sync();
  }
  {
    mvd::FileWriter writer(directory / "mvd.wal");
    writer.append(mvd::encodeCommitRecord(txid, pending.size()));
    writer.sync();
  }
  table.applyPending(pending);
}
void run(const mvd::fs::path &directory) {
  mvd::Table table(definition(), directory / "t1.dat");
  table.setCacheLimit(budget);
  std::map<int64_t, std::optional<mvd::Row>> changes;
  for (int64_t primary = 0; primary < static_cast<int64_t>(count); ++primary) {
    changes.emplace(primary, row(primary));
  }
  commit(table, directory, 1, std::move(changes));
  require(!table.fullyCached(), "fixture did not leave raw row cache");
  const auto *codes = table.diskDistanceCodes(2, 128);
  require(codes && codes->size() == count && codes->code_stride == 128,
          "disk cache was not prepared");
  const auto *storage = codes->codes.data();
  require(table.diskDistanceCodes(2, 128)->codes.data() == storage,
          "warm query rebuilt disk codes");
  require(Access::retained(table) <= budget && Access::disk(table) > 0,
          "disk cache exceeded its table budget");
  size_t visited = 0;
  require(table.forEachScreenedDiskDistanceBase(2, *codes,
      [](size_t i) { return i % 17 == 0; },
      [&](int64_t primary, const std::vector<float> &vector,
          const std::vector<uint8_t> &) {
        require(primary == static_cast<int64_t>(visited * 17),
                "disk screen reordered or misidentified payloads");
        require(vector == row(primary)[2].vector(), "screened payload is stale");
        ++visited;
        return true;
      }), "screened scan unexpectedly fell back");
  require(visited == (count + 16) / 17, "screened scan read rejected payloads");
  for (size_t column : {size_t{1}, size_t{4}}) {
    mvd::Condition predicate;
    predicate.column = column;
    predicate.op = mvd::CompareOp::Eq;
    if (column == 1) predicate.value.setInteger(1);
    else predicate.value.setText("red");
    size_t scanned = 0;
    size_t matched = 0;
    require(table.forEachScreenedDiskDistanceBase(2, *codes,
        [&](size_t i) { ++scanned; return i % 5 != 0; },
        [&](int64_t primary, const std::vector<float> &vector,
            const std::vector<uint8_t> &) {
          require(primary % 5 != 0, "rejected code reached predicate callback");
          require(column == 1 ? primary % 4 == 1 : primary % 3 == 0,
                  "nonmatching disk predicate reached exact TopK callback");
          require(vector == row(primary)[2].vector(), "filtered original vector changed");
          ++matched;
          return true;
        }, &predicate), "filtered scan unexpectedly fell back");
    size_t expected = 0;
    for (size_t i = 0; i < count; ++i) {
      if (i % 5 != 0 && (column == 1 ? i % 4 == 1 : i % 3 == 0)) ++expected;
    }
    require(scanned == count && matched == expected, "filtered survivor count differs");
  }
  mvd::Condition rejected;
  rejected.column = 1;
  rejected.op = mvd::CompareOp::Lt;
  rejected.value.setInteger(1);
  require(!table.forEachScreenedDiskDistanceBase(2, *codes, [](size_t) { return true; },
      [](int64_t, const std::vector<float> &, const std::vector<uint8_t> &) {
        throw std::runtime_error("unsupported predicate partially consumed candidates");
        return true;
      }, &rejected), "non-equality predicate accepted by disk code scan");
  mvd::Condition condition;
  condition.column = 1;
  condition.op = mvd::CompareOp::Eq;
  condition.value.setInteger(1);
  require(table.equalityCandidates(condition) != nullptr, "small equality index was lost");
  require(Access::retained(table) <= budget, "equality and disk caches double-spent budget");
  require(table.diskDistanceCodes(2, 128)->codes.data() == storage,
          "equality change discarded an independent disk matrix");
  condition.column = 0;
  table.equalityCandidates(condition);
  require(Access::retained(table) <= budget, "high-cardinality index ignored disk cache");
  require(table.diskDistanceCodes(3, 65)->code_stride == 65, "vector column switch reused wrong codes");
  table.setCacheLimit(16 * 1024);
  require(Access::disk(table) == 0 && table.diskDistanceCodes(2, 128) == nullptr,
          "cache rebalance retained oversized codes");
  table.setCacheLimit(budget);
  require(table.diskDistanceCodes(2, 128) != nullptr, "larger budget could not rebuild codes");
  commit(table, directory, 2, {{3, row(3, 9)}, {4, std::nullopt}, {600, row(600)}});
  require(Access::disk(table) == 0, "commit left a stale disk matrix");
  codes = table.diskDistanceCodes(2, 128);
  require(codes && codes->size() == count, "mutation rebuild lost rows");
  bool replacement_seen = false;
  table.forEachScreenedDiskDistanceBase(2, *codes, [](size_t) { return true; },
      [&](int64_t primary, const std::vector<float> &vector,
          const std::vector<uint8_t> &) {
        require(primary != 4, "deleted row survived matrix invalidation");
        if (primary == 3) {
          require(vector == row(3, 9)[2].vector(), "updated vector used old payload");
          replacement_seen = true;
        }
        return true;
      });
  require(replacement_seen, "updated row disappeared");
  size_t injected = 0;
  for (long long point = 0; point < 20; ++point) {
    Access::clear(table);
    allocation_injected = false;
    allocation_budget = point;
    const auto *result = table.diskDistanceCodes(2, 128);
    allocation_budget = -1;
    if (allocation_injected) {
      ++injected;
      require(result == nullptr && Access::disk(table) == 0,
              "allocation failure published a partial matrix");
    } else {
      require(result && Access::retained(table) <= budget,
              "successful preparation lost budget accounting");
    }
  }
  require(injected >= 5, "allocation sweep did not exercise preparation");
  Access::clear(table);
  codes = table.diskDistanceCodes(2, 128);
  require(codes != nullptr, "cache could not recover after allocation failures");
  const auto stamp = mvd::fs::last_write_time(table.dataPath());
  {
    std::fstream file(table.dataPath(), std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(-1, std::ios::end);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 1;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
  }
  mvd::fs::last_write_time(table.dataPath(), stamp + std::chrono::seconds(1));
  size_t calls = 0;
  require(!table.forEachScreenedDiskDistanceBase(2, *codes,
      [](size_t) { return false; },
      [&](int64_t, const std::vector<float> &, const std::vector<uint8_t> &) {
        ++calls;
        return true;
      }) && calls == 0, "modified file reused old screening codes");
  require(table.diskDistanceCodes(2, 128) == nullptr && Access::disk(table) == 0,
          "modified file retained stale disk codes");
  bool corrupt = false;
  try {
    table.forEachDiskDistanceBase(2,
        [](int64_t, const std::vector<float> &, const std::vector<uint8_t> &) { return true; });
  } catch (const mvd::DBError &error) {
    corrupt = error.code() == "CORRUPT_DATA";
  }
  require(corrupt, "fallback skipped CRC for a rejected damaged payload");
  std::cout << "PASS disk codes reuse, budget, invalidation, allocation and CRC; injected="
            << injected << '\n';
}
}

int main() {
  const auto directory = mvd::fs::temp_directory_path() /
      ("mvd-disk-codes-unit-" + std::to_string(
#ifdef _WIN32
          _getpid()
#else
          getpid()
#endif
      ));
  mvd::fs::create_directories(directory);
  try {
    run(directory);
    mvd::fs::remove_all(directory);
    return 0;
  } catch (const std::exception &error) {
    allocation_budget = -1;
    std::cerr << error.what() << '\n';
    mvd::fs::remove_all(directory);
    return 1;
  }
}
