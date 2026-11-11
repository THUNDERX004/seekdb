#include <cstdlib>
#include <cstdint>
#include <new>

namespace {
struct ReleasedBuffer { uintptr_t address = 0; bool released = false; };
ReleasedBuffer watched[3];
void noteRelease(void *memory) noexcept {
  for (auto &buffer : watched) {
    if (buffer.address != 0 && buffer.address == reinterpret_cast<uintptr_t>(memory)) {
      buffer.released = true;
    }
  }
}
}
void *operator new(std::size_t bytes) {
  if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
  throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *memory) noexcept { noteRelease(memory); std::free(memory); }
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }

#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
using Model = std::map<int64_t, mvd::Row>;
using Changes = std::map<int64_t, std::optional<mvd::Row>>;
constexpr size_t vector_column = 6, dimension = 17;
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
mvd::TableDef definition() {
  mvd::TableDef result;
  result.oid = 1; result.name = "t"; result.primary_key = 2; result.vector_column = vector_column;
  result.columns = {{"tag", mvd::ColumnType::BigInt, 0}, {"body", mvd::ColumnType::VarChar, 1024},
      {"id", mvd::ColumnType::BigInt, 0}, {"category", mvd::ColumnType::VarChar, 16},
      {"n", mvd::ColumnType::BigInt, 0}, {"empty", mvd::ColumnType::VarChar, 1},
      {"v", mvd::ColumnType::Vector, dimension}, {"tail", mvd::ColumnType::VarChar, 64}};
  return result;
}
mvd::Row makeRow(int64_t primary, size_t seed, bool raw) {
  mvd::Row row(8);
  row[0].setInteger(seed < 257 ? 7 : 8);
  row[1].setText(std::string(96, char('a' + seed % 26)) + '\0' + "quote'\\;\xc3\xa9\xe4\xb8\xad" + std::to_string(seed));
  row[2].setInteger(primary); row[3].setText(seed % 2 ? "red" : "blue");
  row[4].setInteger(seed % 2 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max());
  row[5].setText(""); row[7].setText("tail:" + std::to_string(seed));
  std::vector<float> values(dimension, 0);
  if (raw) {
    constexpr std::array<uint32_t, 12> boundaries{{0, 0x80000000U, 1, 0x80000001U,
        0x007fffffU, 0x807fffffU, 0x00800000U, 0x80800000U,
        0x7f7fffffU, 0xff7fffffU, 0x3f800000U, 0xbf800000U}};
    uint32_t state = 0x9e3779b9U ^ static_cast<uint32_t>(seed + 1);
    for (size_t axis = 0; axis < dimension; ++axis) {
      state ^= state << 13; state ^= state >> 17; state ^= state << 5;
      uint32_t bits = axis < boundaries.size() ? boundaries[(axis + seed) % boundaries.size()] : state;
      if ((bits & 0x7f800000U) == 0x7f800000U) bits ^= 0x00800000U;
      values[axis] = std::bit_cast<float>(bits);
    }
  } else {
    values[0] = static_cast<float>(int(seed * 17 % 37) - 18);
    values[1] = static_cast<float>(int(seed * 11 % 29) - 14);
    if (primary == 3) values[0] = values[1] = 0;  // Last physical row must be competitive.
  }
  row[vector_column].setVector(std::move(values));
  return row;
}
template <typename Actual>
void checkRow(const Actual &actual, const mvd::Row &expected) {
  require(actual.size() == expected.size(), "packed column count changed");
  for (size_t column = 0; column < expected.size(); ++column) {
    require(actual[column].integer() == expected[column].integer() &&
                std::string_view(actual[column].text()) == expected[column].text(),
            "packed integer/text/inactive fields changed");
    const auto &wanted = expected[column].vector();
    const auto &values = actual[column].vector();
    require(values.size() == wanted.size(), "packed vector dimension changed");
    for (size_t axis = 0; axis < wanted.size(); ++axis) {
      require(std::bit_cast<uint32_t>(values[axis]) == std::bit_cast<uint32_t>(wanted[axis]),
              "packed finite float32 bits changed");
    }
  }
}
void publish(mvd::Table &table, Changes &changes, uint64_t txid, const mvd::fs::path &directory) {
  mvd::FileWriter writer(table.dataPath());
  auto pending = table.appendChanges(txid, writer, changes);
  writer.sync(table.fullyCached());
  mvd::FileWriter wal(directory / "mvd.wal");
  wal.append(mvd::encodeCommitRecord(txid, pending.size())); wal.sync();
  table.applyPending(pending);
}
void checkScan(mvd::Table &table, const Model &expected) {
  std::set<int64_t> visited;
  require(table.fullyCached() && table.rowCount() == expected.size(), "packed cache lost residency/count");
  table.forEachCached([&](int64_t primary, mvd::RowView row, bool stable) {
    require(stable && row.isPacked() && visited.insert(primary).second, "cached view is not stable/packed/unique");
    checkRow(row, expected.at(primary));
    checkRow(row.materialize(), expected.at(primary));
    return true;
  });
  require(visited.size() == expected.size(), "cached scan omitted rows");
}
void checkPublication(const mvd::fs::path &directory) {
  mvd::fs::create_directories(directory);
  Model model;
  {
    mvd::Table table(definition(), directory / "t.dat"); table.setCacheLimit(16 * 1024 * 1024);
    {
      Changes overlay;
      for (int64_t primary = 0; primary < 257; ++primary) {
        model.emplace(primary, makeRow(primary, static_cast<size_t>(primary), true));
        overlay.emplace(primary, model.at(primary));
      }
      const auto &source = *overlay.at(128);
      watched[0] = {reinterpret_cast<uintptr_t>(source.data()), false};
      watched[1] = {reinterpret_cast<uintptr_t>(source[1].text().data()), false};
      watched[2] = {reinterpret_cast<uintptr_t>(source[vector_column].vector().data()), false};
      publish(table, overlay, 1, directory);
      // Publication copies into packed storage once, then frees the owning
      // overlay's buffers. This deliberately replaces the old zero-copy rule.
      for (const auto &buffer : watched) require(buffer.released, "publication retained an owning source allocation");
      for (const auto &[key, row] : overlay) { (void)key; require(row->empty(), "packed source was not released"); }
      table.forEachCached([&](int64_t key, mvd::RowView row, bool) {
        if (key == 128) {
          require(reinterpret_cast<uintptr_t>(row[1].text().data()) != watched[1].address &&
                      reinterpret_cast<uintptr_t>(row[vector_column].vector().data()) != watched[2].address,
                  "packed view still borrows source text/vector storage");
        }
        return true;
      });
      for (auto &buffer : watched) buffer = {};
    }
    checkScan(table, model);  // Both overlay and PendingRecord pointers are gone.
    std::optional<mvd::RowView> borrowed;
    table.forEachCached([&](int64_t key, mvd::RowView row, bool stable) {
      if (key == 128) { require(stable, "borrowed view is transient"); borrowed = row; }
      return true;
    });
    require(borrowed.has_value(), "borrowed fixture missing");
    const float *vector = (*borrowed)[vector_column].vector().data();
    const char *text = (*borrowed)[1].text().data();
    const mvd::Row snapshot = borrowed->materialize();
    require(snapshot[vector_column].vector().data() != vector && snapshot[1].text().data() != text,
            "materialize did not create independent owning storage");
    for (int column : {-1, 0, 3, -1, 0}) {
      mvd::Condition condition;
      condition.column = column < 0 ? 0 : static_cast<size_t>(column); condition.op = mvd::CompareOp::Eq;
      if (column == 3) condition.value.setText("red"); else condition.value.setInteger(7);
      const auto *indexed = table.cachedDistanceCandidates(vector_column, column < 0 ? nullptr : &condition);
      require(indexed != nullptr, "packed selection directory unavailable");
      size_t seen = 0, wanted = 0;
      for (const auto &[key, row] : model) {
        (void)key;
        if (column < 0 || (column == 0 ? row[0].integer() == 7 : row[3].text() == "red")) ++wanted;
      }
      table.forEachIndexedBase(*indexed, nullptr, [&](int64_t key, mvd::RowView row, bool stable) {
        require(stable && row.isPacked(), "indexed selection materialized a cached row");
        checkRow(row, model.at(key)); ++seen; return true;
      });
      require(seen == wanted, "equality switch lost packed rows");
      checkScan(table, model);
      checkRow(*borrowed, model.at(128));
      require((*borrowed)[vector_column].vector().data() == vector && (*borrowed)[1].text().data() == text,
              "selection/index construction invalidated a stable packed view");
    }
    borrowed.reset();  // Never dereference an old borrowed view after invalidation.
    Changes changes;
    changes.emplace(128, makeRow(128, 1001, true)); changes.emplace(1, std::nullopt);
    model[128] = *changes.at(128); model.erase(1); publish(table, changes, 2, directory);
    checkScan(table, model); checkRow(snapshot, makeRow(128, 128, true));
    changes.clear(); changes.emplace(1, makeRow(1, 999, true)); model[1] = *changes.at(1);
    publish(table, changes, 3, directory); checkScan(table, model);
  }
  const auto wal = mvd::scanCommitLog(directory / "mvd.wal"); uint64_t next = 0;
  mvd::Table recovered(definition(), directory / "t.dat"); recovered.setCacheLimit(16 * 1024 * 1024);
  recovered.load(wal.committed, next); checkScan(recovered, model);
  for (const auto &[key, expected] : model) {
    mvd::Row row; require(recovered.findBase(key, row), "restart omitted a packed row"); checkRow(row, expected);
  }
}

std::string quote(std::string_view text) {
  std::string result = "'";
  for (char byte : text) { if (byte == '\'' || byte == '\\') result.push_back(byte); result.push_back(byte); }
  return result + "'";
}
std::string integerVector(const mvd::Row &row) {
  std::string result = "[";
  for (float value : row[vector_column].vector()) {
    if (result.size() != 1) result += ',';
    result += std::to_string(static_cast<int>(value));
  }
  return result + ']';
}
std::vector<std::string> fields(const mvd::Row &row) {
  return {std::to_string(row[0].integer()), row[1].text(), std::to_string(row[2].integer()),
      row[3].text(), std::to_string(row[4].integer()), row[5].text(), integerVector(row), row[7].text()};
}
std::string tuple(const mvd::Row &row) {
  const auto values = fields(row);
  return "(" + values[0] + ',' + quote(values[1]) + ',' + values[2] + ',' + quote(values[3]) + ',' +
      values[4] + ',' + quote(values[5]) + ',' + quote(values[6]) + ',' + quote(values[7]) + ')';
}
constexpr const char *projection = "tag,body,id,category,n,empty,v,tail";
void checkQueries(mvd::Database &database, const Model &model, bool full_first) {
  const std::string probe = integerVector(makeRow(3, 0, false));
  size_t query = 0;
  for (int filter : {full_first ? -1 : 0, -1, 0, 3, 1, -1, 0}) {
    const size_t skip = query++ % 2 ? 2 : 0;
    std::vector<std::pair<uint64_t, int64_t>> exact;
    for (const auto &[key, row] : model) {
      if ((filter == 0 && row[0].integer() != 7) || (filter == 1 && row[0].integer() != 8) ||
          (filter == 3 && row[3].text() != "red")) continue;
      uint64_t squared = 0;
      // Fixture coordinates are small integers: this independent integer sum
      // is the exact float32 squared distance, including ties resolved by PK.
      for (float value : row[vector_column].vector()) {
        const int64_t integer = static_cast<int64_t>(value); squared += static_cast<uint64_t>(integer * integer);
      }
      exact.emplace_back(squared, key);
    }
    std::sort(exact.begin(), exact.end());
    const std::string where = filter < 0 ? "" : filter == 3 ? " WHERE category='red'" :
        filter == 0 ? " WHERE tag=7" : " WHERE tag=8";
    auto result = database.execute("SELECT " + std::string(projection) + ",L2_DISTANCE(v," + quote(probe) +
        ") AS d FROM t" + where + " ORDER BY d,id LIMIT 7 OFFSET " + std::to_string(skip));
    const size_t count = exact.size() > skip ? std::min<size_t>(7, exact.size() - skip) : 0;
    require(result && result->select && !result->spool && result->rows.size() == count, "packed TopK count changed");
    for (size_t index = 0; index < count; ++index) {
      const auto &[squared, key] = exact[index + skip]; const auto expected = fields(model.at(key));
      require(result->rows[index].size() == expected.size() + 1 &&
                  std::equal(expected.begin(), expected.end(), result->rows[index].begin()),
              "packed TopK order or projected fields differ from independent exact oracle");
      const double distance = std::stod(result->rows[index].back());
      require(std::abs(distance - std::sqrt(static_cast<double>(squared))) <= 1e-5,
              "packed TopK displayed distance changed");
    }
  }
  size_t offset = 0;
  for (auto expected = model.begin(); expected != model.end();) {
    auto result = database.execute("SELECT " + std::string(projection) + " FROM t ORDER BY id LIMIT 64 OFFSET " + std::to_string(offset));
    const size_t count = std::min<size_t>(64, model.size() - offset);
    require(result && !result->spool && result->rows.size() == count, "all-fields scan omitted packed rows");
    for (size_t index = 0; index < count; ++index, ++expected) {
      require(result->rows[index] == fields(expected->second), "all-fields scan/restart changed packed contents");
    }
    offset += count;
  }
}
void checkDatabase(const mvd::fs::path &directory, size_t count) {
  Model model;
  {
    mvd::Database database(directory, false);
    database.execute("CREATE TABLE t(tag BIGINT,body VARCHAR(1024),id BIGINT PRIMARY KEY,category VARCHAR(16),"
                     "n BIGINT,empty VARCHAR(1),v VECTOR(17),tail VARCHAR(64))");
    std::string insert = "INSERT INTO t VALUES";
    for (size_t index = 0; index < count; ++index) {
      const int64_t key = static_cast<int64_t>((count - index) * 3);
      auto row = makeRow(key, index, false); if (index) insert += ',';
      insert += tuple(row); model.emplace(key, std::move(row));
    }
    database.execute(insert); database.execute("CREATE VECTOR INDEX ix ON t(v)");
    checkQueries(database, model, count == 2049);
    const int64_t changed = static_cast<int64_t>((count / 2 + 1) * 3);
    auto replacement = makeRow(changed, 4001, false);
    const auto values = fields(replacement);
    database.execute("BEGIN");
    database.execute("UPDATE t SET tag=" + values[0] + ",body=" + quote(values[1]) + ",category=" + quote(values[3]) +
        ",n=" + values[4] + ",empty=" + quote(values[5]) + ",v=" + quote(values[6]) + ",tail=" + quote(values[7]) +
        " WHERE id=" + std::to_string(changed));
    database.execute("DELETE FROM t WHERE id=3"); model[changed] = replacement; model.erase(3);
    checkQueries(database, model, false); database.execute("COMMIT"); checkQueries(database, model, true);
    auto reinserted = makeRow(3, 5001, false); database.execute("INSERT INTO t VALUES" + tuple(reinserted));
    model.emplace(3, std::move(reinserted)); checkQueries(database, model, false);
  }
  mvd::Database recovered(directory, false); checkQueries(recovered, model, count != 2049);
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) { std::cerr << "usage: test_packed_cache EMPTY_TEMP_DIRECTORY\n"; return 2; }
  const mvd::fs::path root = argv[1];
  if (mvd::fs::exists(root) && !mvd::fs::is_empty(root)) { std::cerr << "test directory must be empty\n"; return 2; }
  try {
    // PackedRow allocation-failure strong guarantees have a separate unit
    // harness; test_commit_allocation covers Database RecoveryRequired.
    checkPublication(root / "publication");
    for (size_t count : {257U, 2049U}) checkDatabase(root / std::to_string(count), count);
    std::cout << "PASS packed cache publication/release, finite bits, stable views, exact TopK, mutations/restart\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
