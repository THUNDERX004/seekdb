#include <cstddef>

#define MVD_TEST_PHYSICAL_ORDER
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main
#undef MVD_TEST_PHYSICAL_ORDER

namespace {
using IndexedRow = mvd::Table::IndexedRow;
const mvd::CachedRow constructor_owner{};
constexpr float constructor_vector[] = {1.0F};
constexpr uint64_t disk_offset = 0xfedcba9876543210ULL;

static_assert(std::is_standard_layout_v<IndexedRow>);
static_assert(std::is_trivially_copyable_v<IndexedRow>);
static_assert(std::is_nothrow_copy_constructible_v<IndexedRow>);
static_assert(std::is_nothrow_move_constructible_v<IndexedRow>);
static_assert(std::is_nothrow_copy_assignable_v<IndexedRow>);
static_assert(std::is_nothrow_move_assignable_v<IndexedRow>);
static_assert(offsetof(IndexedRow, primary) == 0);
static_assert(offsetof(IndexedRow, offset) == offsetof(IndexedRow, cached));
static_assert(sizeof(void *) != 8 || sizeof(IndexedRow) == 24);
static_assert(sizeof(void *) != 8 || alignof(IndexedRow) == 8);
static_assert(sizeof(void *) != 8 || offsetof(IndexedRow, offset) == 8);
static_assert(sizeof(void *) != 8 || offsetof(IndexedRow, vector_data) == 16);

// Constant evaluation rejects inactive-member reads. These assertions prove
// the selected member, rather than merely comparing the union's byte layout.
constexpr IndexedRow default_row{};
static_assert(default_row.primary == 0 && default_row.offset == 0 &&
              default_row.vector_data == nullptr);
constexpr IndexedRow directIndexedRow{7, disk_offset, &constructor_owner, nullptr};
static_assert(directIndexedRow.primary == 7 &&
              directIndexedRow.cached == &constructor_owner &&
              directIndexedRow.vector_data == nullptr);
constexpr IndexedRow directDiskRow{-7, disk_offset, nullptr, constructor_vector};
static_assert(directDiskRow.primary == -7 && directDiskRow.offset == disk_offset &&
              directDiskRow.vector_data == constructor_vector);
constexpr std::array<IndexedRow, 4> default_batch{};
static_assert(default_batch[3].offset == 0);

constexpr bool copyAndMoveKeepActiveMember() {
  IndexedRow row;
  row = directIndexedRow;
  const IndexedRow copied(row);
  IndexedRow moved(std::move(row));
  if (copied.cached != &constructor_owner || moved.cached != &constructor_owner) {
    return false;
  }
  moved = directDiskRow;
  row = std::move(moved);
  if (row.offset != disk_offset || row.vector_data != constructor_vector) {
    return false;
  }
  IndexedRow cached = directIndexedRow;
  std::swap(row, cached);
  return row.cached == &constructor_owner && row.vector_data == nullptr &&
      cached.offset == disk_offset && cached.vector_data == constructor_vector;
}
static_assert(copyAndMoveKeepActiveMember());

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

namespace mvd {
struct PhysicalOrderTest {
  static bool uniform(const Table &table) {
    for (const auto &[primary, version] : table.versions_) {
      (void)primary;
      if (version.cached != table.cache_enabled_) return false;
    }
    return true;
  }
  static bool invalidated(const Table &table) {
    return !table.cached_distance_ready_ && table.cached_distance_rows_.empty() &&
        table.cached_distance_rows_.codes.empty() &&
        table.integer_equality_index_.empty() && table.text_equality_index_.empty() &&
        table.disk_distance_rows_.empty() && table.disk_distance_rows_.codes.empty() &&
        table.distance_mask_source_ == nullptr && table.distance_mask_.empty() &&
        table.equality_bytes_ == 0 && table.quantized_bytes_ == 0 &&
        table.disk_distance_bytes_ == 0;
  }
  static bool cachedMembers(const Table &table, const Table::IndexedRows &rows) {
    if (!table.cache_enabled_) return false;
    for (const auto &indexed : rows) {
      const auto found = table.versions_.find(indexed.primary);
      if (found == table.versions_.end() || indexed.cached != &found->second.row) {
        return false;
      }
    }
    return true;
  }
};
}  // namespace mvd

namespace {
using Access = mvd::PhysicalOrderTest;
constexpr size_t dimension = 17;

mvd::TableDef definition() {
  mvd::TableDef result;
  result.oid = 1;
  result.columns = {{"id", mvd::ColumnType::BigInt, 0},
                    {"tag", mvd::ColumnType::BigInt, 0},
                    {"v", mvd::ColumnType::Vector, dimension}};
  result.vector_column = 2;
  return result;
}
mvd::Row makeRow(int64_t primary, size_t axes = dimension) {
  mvd::Row row(3);
  row[0].setInteger(primary);
  row[1].setInteger(primary % 2);
  row[2].setVector(std::vector<float>(axes, static_cast<float>(primary + 1)));
  return row;
}
void publish(mvd::Table &table, int64_t primary, size_t axes = dimension) {
  auto row = makeRow(primary, axes);
  table.applyPending({{1, primary, 1, &row, uint64_t(primary + 1) * 64}});
}

void checkContainerCopies(bool cached) {
  mvd::Table::IndexedRows rows;
  // Force reallocation, sorting and directory swap for each homogeneous mode.
  for (int64_t primary = 31; primary >= 0; --primary) {
    rows.push_back({primary, disk_offset + uint64_t(primary),
                    cached ? &constructor_owner : nullptr,
                    primary % 2 ? nullptr : constructor_vector});
  }
  std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
    return left.primary < right.primary;
  });
  mvd::Table::IndexedRows copied = rows;
  mvd::Table::IndexedRows moved;
  copied.swap(moved);
  require(copied.empty() && moved.size() == 32, "directory swap lost entries");
  for (size_t i = 0; i < moved.size(); ++i) {
    const auto &row = moved[i];
    require(row.primary == static_cast<int64_t>(i), "directory sort lost identity");
    require(row.vector_data == (i % 2 ? nullptr : constructor_vector),
            "directory copy changed vector metadata");
    if (cached) {
      require(row.cached == &constructor_owner,
              "copy/move changed a cached member");
    } else {
      require(row.offset == disk_offset + i,
              "copy/move changed a disk member");
    }
  }
}

void checkNullVectorCachedDispatch() {
  const auto def = definition();
  mvd::Table table(def, "unused-compact-null-vector.dat");
  table.setCacheLimit(1024 * 1024);
  mvd::CachedRow owner;
  auto row = makeRow(7);
  require(mvd::publishCachedRow(owner, row, mvd::cachedRowLayout(def)),
          "cached fixture could not be published");
  mvd::Table::IndexedRows direct;
  direct.push_back({7, disk_offset, &owner, nullptr});
  size_t visited = 0;
  require(table.forEachIndexedBase(direct, nullptr,
      [&](int64_t primary, mvd::RowView view, bool stable) {
        require(stable && primary == 7 && view[0].integer() == 7 &&
                    view[2].vector().size() == dimension,
                "null vector metadata changed cached dispatch");
        ++visited;
        return true;
      }) && visited == 1, "cached scan did not visit its null-vector entry");
}

void checkInvalidation() {
  mvd::Table table(definition(), "unused-compact-invalidation.dat");
  constexpr size_t budget = 1024 * 1024;
  table.setCacheLimit(budget);
  for (int64_t key = 0; key < 4; ++key) publish(table, key);
  mvd::Condition condition;
  condition.column = 1;
  condition.op = mvd::CompareOp::Eq;
  condition.value.setInteger(1);
  const auto warm = [&] {
    const auto *full = table.cachedDistanceCandidates(2, nullptr);
    const auto *bucket = table.equalityCandidates(condition);
    require(full != nullptr && full->size() == table.rowCount() && bucket != nullptr &&
                Access::uniform(table) && Access::cachedMembers(table, *full) &&
                Access::cachedMembers(table, *bucket),
            "cached directory contains a different active member");
  };
  warm();
  condition.column = 0;
  warm();  // clearEqualityIndex(true) preserves the fully cached directory.
  publish(table, 1);
  require(Access::invalidated(table) && Access::uniform(table),
          "replacement retained a borrowed directory");
  warm();
  table.applyPending({{1, 2, 3, nullptr, 640}});
  require(Access::invalidated(table) && Access::uniform(table),
          "delete retained a borrowed directory");
  warm();
  table.setCacheLimit(budget);
  require(table.fullyCached() && Access::invalidated(table),
          "cache rebalance retained a borrowed directory");
  warm();
  uint64_t next_txid = 0;
  table.load({}, next_txid);  // No data file: exercise reload invalidation only.
  require(Access::invalidated(table) && Access::uniform(table),
          "reload retained a borrowed directory");
  warm();
  table.setCacheLimit(0);
  require(!table.fullyCached() && Access::invalidated(table) && Access::uniform(table) &&
              table.cachedDistanceCandidates(2, nullptr) == nullptr,
          "cache disable retained cached directory members");
  table.setCacheLimit(budget);
  require(!table.fullyCached() && Access::uniform(table),
          "cache budget growth created mixed residency");
}

void checkMismatchedVector() {
  mvd::Table table(definition(), "unused-compact-mismatched-vector.dat");
  table.setCacheLimit(1024 * 1024);
  publish(table, 0, dimension - 1);
  require(Access::uniform(table), "vector fallback created mixed residency");
#ifdef MVD_OWNING_ROW_CACHE
  const auto *rows = table.cachedDistanceCandidates(2, nullptr);
  require(table.fullyCached() && rows != nullptr && rows->size() == 1 &&
              (*rows)[0].vector_data == nullptr && Access::cachedMembers(table, *rows),
          "mismatched cached vector did not retain its cached member");
  size_t visited = 0;
  require(table.forEachIndexedBase(*rows, nullptr,
      [&](int64_t, mvd::RowView row, bool stable) {
        require(stable && row[2].vector().size() == dimension - 1,
                "mismatched cached vector was treated as a disk offset");
        ++visited;
        return true;
      }) && visited == 1, "mismatched cached row was omitted");
#else
  require(!table.fullyCached() && Access::invalidated(table),
          "unsupported packed row did not invalidate cached directories");
#endif
}
}  // namespace

int main() {
  try {
    checkContainerCopies(false);
    checkContainerCopies(true);
    checkNullVectorCachedDispatch();
    checkInvalidation();
    checkMismatchedVector();
    std::cout << "PASS compact IndexedRow layout, active members and invalidation\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL compact IndexedRow: " << error.what() << '\n';
    return 1;
  }
}
