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
#define MVD_TEST_PHYSICAL_ORDER
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main
#undef MVD_TEST_PHYSICAL_ORDER

namespace mvd {
struct PhysicalOrderTest {
  using Directory = std::vector<std::pair<uint64_t, int64_t>>;

  static const Directory &retained(const Table &table) {
    return table.physical_order_;
  }
  static bool dirty(const Table &table) { return table.physical_order_dirty_; }
  static const Directory &materialize(const Table &table) {
    return table.physicalOrder();
  }
};
}  // namespace mvd

namespace {
using Access = mvd::PhysicalOrderTest;
using Changes = std::map<int64_t, std::optional<mvd::Row>>;
using Revisions = std::map<int64_t, std::optional<int64_t>>;
struct ExpectedRow {
  int64_t revision;
  uint64_t offset;
};
using ExpectedRows = std::map<int64_t, ExpectedRow>;
constexpr int64_t row_count = 10000;
constexpr int64_t batch_size = 100;
constexpr size_t dimension = 128;

void require(bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

mvd::TableDef definition() {
  mvd::TableDef result;
  result.oid = 1;
  result.name = "lazy";
  result.columns = {{"id", mvd::ColumnType::BigInt, 0},
                    {"cat", mvd::ColumnType::VarChar, 16},
                    {"payload", mvd::ColumnType::VarChar, 64},
                    {"emb", mvd::ColumnType::Vector, dimension}};
  result.vector_column = 3;
  return result;
}

std::string category(int64_t primary) {
  return "c" + std::to_string((primary % 10 + 10) % 10);
}

std::string payload(int64_t primary, int64_t revision) {
  return "row" + std::to_string(primary) + "/" + std::to_string(revision);
}

float component(int64_t primary, int64_t revision, size_t axis) {
  return static_cast<float>((primary * 3 + revision * 17 +
                             static_cast<int64_t>(axis)) % 97) / 8.0F;
}

mvd::Row makeRow(int64_t primary, int64_t revision) {
  mvd::Row row(4);
  row[0].setInteger(primary);
  row[1].setText(category(primary));
  row[2].setText(payload(primary, revision));
  std::vector<float> vector(dimension);
  for (size_t axis = 0; axis < dimension; ++axis) {
    vector[axis] = component(primary, revision, axis);
  }
  row[3].setVector(std::move(vector));
  return row;
}

void checkRow(const mvd::Row &row, int64_t primary, int64_t revision) {
  require(row.size() == 4 && row[0].integer() == primary &&
              row[1].text() == category(primary) &&
              row[2].text() == payload(primary, revision) &&
              row[3].vector().size() == dimension,
          "scan returned a stale or malformed row");
  for (size_t axis = 0; axis < dimension; ++axis) {
    require(std::bit_cast<uint32_t>(row[3].vector()[axis]) ==
                std::bit_cast<uint32_t>(component(primary, revision, axis)),
            "scan changed vector component bits");
  }
}

void checkUnmaterialized(const mvd::Table &table) {
  require(Access::dirty(table) && Access::retained(table).empty() &&
              Access::retained(table).capacity() == 0,
          "an unused physical directory retained storage or became clean");
}

// Exercise the production append -> data sync -> WAL sync -> publication order.
void commit(mvd::Table &table, const mvd::fs::path &directory, uint64_t txid,
            const Revisions &revisions, ExpectedRows &expected) {
  Changes changes;
  for (const auto &[primary, revision] : revisions) {
    if (revision.has_value()) {
      changes.emplace(primary, makeRow(primary, *revision));
    } else {
      changes.emplace(primary, std::nullopt);
    }
  }
  std::vector<mvd::PendingRecord> pending;
  {
    mvd::FileWriter writer(table.dataPath());
    pending = table.appendChanges(txid, writer, changes);
    writer.sync(table.fullyCached());
  }
  require(pending.size() == revisions.size(), "append lost a mutation");
  {
    mvd::FileWriter writer(directory / "mvd.wal");
    writer.append(mvd::encodeCommitRecord(txid, pending.size()));
    writer.sync();
  }
  table.applyPending(pending);
  for (const auto &record : pending) {
    const auto &revision = revisions.at(record.primary);
    const uint16_t expected_op = !revision.has_value() ? 3 :
        expected.contains(record.primary) ? 2 : 1;
    require(record.op == expected_op, "mutation changed its insert/update/delete op");
    if (revision.has_value()) {
      expected[record.primary] = {*revision, record.offset};
    } else {
      expected.erase(record.primary);
    }
  }
  require(table.rowCount() == expected.size(), "publication changed live row count");
}

Access::Directory expectedOrder(const ExpectedRows &expected) {
  Access::Directory result;
  result.reserve(expected.size());
  for (const auto &[primary, row] : expected) {
    result.emplace_back(row.offset, primary);
  }
  std::sort(result.begin(), result.end());
  return result;
}

void checkDiskScan(const mvd::Table &table, const ExpectedRows &expected) {
  require(!table.fullyCached(), "disk fixture still has cached rows");
  const auto wanted = expectedOrder(expected);
  size_t visited = 0;
  const bool complete = table.forEachBase(
      [&](int64_t primary, const mvd::Row &row, bool stable) {
        require(!stable && visited < wanted.size() &&
                    primary == wanted[visited].second,
                "disk scan duplicated, omitted or misordered a current offset");
        checkRow(row, primary, expected.at(primary).revision);
        ++visited;
        return true;
      }, nullptr, true);
  require(complete && visited == wanted.size() && !Access::dirty(table),
          "disk scan did not finish a complete physical directory");
}

void checkFirstRebuildFailure(mvd::Table &table, const ExpectedRows &expected) {
  checkUnmaterialized(table);
  bool failed = false;
  allocation_calls = 0;
  allocation_injected = false;
  allocation_budget = 0;
  try {
    (void)Access::materialize(table);
  } catch (const std::bad_alloc &) {
    failed = true;
  } catch (...) {
    allocation_budget = -1;
    throw;
  }
  allocation_budget = -1;
  require(failed && allocation_injected && allocation_calls == 1,
          "first physical-directory reserve failure was not injected");
  checkUnmaterialized(table);
  require(table.rowCount() == expected.size(), "failed rebuild changed live rows");

  checkDiskScan(table, expected);
  require(Access::retained(table) == expectedOrder(expected),
          "retry published stale, missing or unsorted offsets");

  const auto *storage = Access::retained(table).data();
  const auto capacity = Access::retained(table).capacity();
  allocation_calls = 0;
  allocation_injected = false;
  allocation_budget = 0;
  try {
    (void)Access::materialize(table);
  } catch (...) {
    allocation_budget = -1;
    throw;
  }
  allocation_budget = -1;
  require(!allocation_injected && allocation_calls == 0 &&
              Access::retained(table).data() == storage &&
              Access::retained(table).capacity() == capacity,
          "clean physical directory rebuilt or allocated on reuse");
}

void checkLazyPhysicalOrder(const mvd::fs::path &directory) {
  mvd::fs::create_directories(directory);
  ExpectedRows expected;
  uint64_t next_txid = 1;
  {
    mvd::Table table(definition(), directory / "t1.dat");
    table.setCacheLimit(64 * 1024 * 1024);
    checkUnmaterialized(table);
    // Reverse the batch key ranges so physical order differs from key order.
    for (int64_t start = 0; start < row_count; start += batch_size) {
      Revisions revisions;
      for (int64_t i = 0; i < batch_size; ++i) {
        revisions.emplace(row_count - start - i, 0);
      }
      commit(table, directory, next_txid++, revisions, expected);
      require(table.fullyCached(), "10k x 128D fixture exceeded its row cache");
      checkUnmaterialized(table);
    }
    commit(table, directory, next_txid++,
           {{17, 1}, {23, std::nullopt}, {-7, 1}}, expected);
    checkUnmaterialized(table);
    commit(table, directory, next_txid++, {{23, 2}}, expected);
    checkUnmaterialized(table);
    size_t visited = 0;
    table.forEachCached([&](int64_t primary, const mvd::Row &row, bool stable) {
      require(stable, "cached fixture returned a temporary row");
      checkRow(row, primary, expected.at(primary).revision);
      ++visited;
      return true;
    });
    require(visited == expected.size(), "cached scan lost a committed row");
    checkUnmaterialized(table);

    table.setCacheLimit(0);
    require(!table.fullyCached(), "zero cache budget did not evict row payloads");
    checkFirstRebuildFailure(table, expected);

    // Once materialized, preserve append maintenance and stale-entry skipping.
    const auto previous_size = Access::retained(table).size();
    const auto old_update_offset = expected.at(17).offset;
    const auto old_delete_offset = expected.at(23).offset;
    commit(table, directory, next_txid++,
           {{17, 2}, {23, std::nullopt}}, expected);
    require(!Access::dirty(table) &&
                Access::retained(table).size() == previous_size + 1 &&
                std::find(Access::retained(table).begin(),
                          Access::retained(table).end(),
                          std::pair<uint64_t, int64_t>{old_update_offset, 17}) !=
                    Access::retained(table).end() &&
                std::find(Access::retained(table).begin(),
                          Access::retained(table).end(),
                          std::pair<uint64_t, int64_t>{old_delete_offset, 23}) !=
                    Access::retained(table).end(),
            "materialized-directory maintenance discarded its stale-entry path");
    checkDiskScan(table, expected);
    commit(table, directory, next_txid++, {{23, 3}, {row_count + 1, 1}}, expected);
    require(!Access::dirty(table) &&
                Access::retained(table).size() == previous_size + 3,
            "reinsert failed to extend the already materialized directory");
    checkDiskScan(table, expected);
  }

  const auto wal = mvd::scanCommitLog(directory / "mvd.wal");
  require(wal.committed.size() == next_txid - 1,
          "restart lost a durable commit marker");
  uint64_t max_seen = 0;
  mvd::Table recovered(definition(), directory / "t1.dat");
  recovered.setCacheLimit(64 * 1024 * 1024);
  recovered.load(wal.committed, max_seen);
  require(recovered.fullyCached() && recovered.rowCount() == expected.size(),
          "recovery changed residency or current row count");
  checkUnmaterialized(recovered);
  recovered.setCacheLimit(0);
  checkUnmaterialized(recovered);
  checkDiskScan(recovered, expected);
  require(Access::retained(recovered) == expectedOrder(expected),
          "recovered directory retained historical or unsorted offsets");
  std::cout << "PASS lazy 10k x 128D cached commits, eviction, reserve failure/retry, "
               "offset order, stale updates/deletes/reinsert and durable restart\n";
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_lazy_physical_order EMPTY_TEMP_DIRECTORY\n";
    return 2;
  }
  try {
    const mvd::fs::path root = argv[1];
    if (mvd::fs::exists(root) && !mvd::fs::is_empty(root)) {
      std::cerr << "test directory must be empty\n";
      return 2;
    }
    checkLazyPhysicalOrder(root);
  } catch (const std::exception &error) {
    allocation_budget = -1;
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
