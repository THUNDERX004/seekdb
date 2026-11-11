#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
using Changes = std::map<int64_t, std::optional<mvd::Row>>;
using Rows = std::vector<std::vector<std::string>>;
constexpr std::array<uint32_t, 6> vector_bits{
    0x80000000U, 0x00000001U, 0x80000001U,
    0x3f800000U, 0x7f7fffffU, 0xff7fffffU};

void require(bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

mvd::TableDef definition(uint32_t oid) {
  mvd::TableDef result;
  result.oid = oid;
  result.name = "t" + std::to_string(oid);
  result.columns = {{"id", mvd::ColumnType::BigInt, 0},
                    {"n", mvd::ColumnType::BigInt, 0},
                    {"body", mvd::ColumnType::VarChar, 4096},
                    {"empty", mvd::ColumnType::VarChar, 1},
                    {"v", mvd::ColumnType::Vector, vector_bits.size()}};
  result.vector_column = 4;
  return result;
}

mvd::Row makeRow(int64_t primary, int64_t number, const std::string &body) {
  mvd::Row row;
  row.reserve(12);  // Retained capacity must move with the row allocation.
  row.resize(5);
  row[0].setInteger(primary);
  row[1].setInteger(number);
  row[2].setText(body);
  row[3].setText("");
  std::vector<float> vector;
  for (uint32_t bits : vector_bits) {
    vector.push_back(std::bit_cast<float>(bits));
  }
  row[4].setVector(std::move(vector));
  return row;
}

void checkRow(const mvd::Row &row, int64_t primary, int64_t number,
              const std::string &body) {
  require(row.size() == 5, "committed column count changed");
  require(row[0].integer() == primary && row[1].integer() == number,
          "committed integer fields changed");
  require(row[2].text() == body && row[3].text().empty(),
          "allocated, empty, NUL or UTF-8 text changed");
  require(row[4].vector().size() == vector_bits.size(),
          "committed vector dimension changed");
  for (size_t i = 0; i < vector_bits.size(); ++i) {
    require(std::bit_cast<uint32_t>(row[4].vector()[i]) == vector_bits[i],
            "committed float32 bits changed");
  }
}

struct Storage {
  const mvd::Value *fields;
  const char *text;
  const float *vector;
  size_t bytes;

  explicit Storage(const mvd::Row &row)
      : fields(row.data()), text(row[2].text().data()),
        vector(row[4].vector().data()), bytes(mvd::cachedRowBytes(row)) {}

  void check(const mvd::Row &row) const {
    require(row.data() == fields && row[2].text().data() == text &&
                row[4].vector().data() == vector,
            "publication copied row, text or vector storage");
    require(mvd::cachedRowBytes(row) == bytes,
            "publication changed the owned row's cache accounting");
  }
};

std::vector<mvd::PendingRecord> appendAndSync(mvd::Table &table,
                                             Changes &changes, uint64_t txid) {
  mvd::FileWriter writer(table.dataPath());
  auto pending = table.appendChanges(txid, writer, changes);
  require(pending.size() == changes.size(), "append lost pending metadata");
  for (const auto &record : pending) {
    require(record.row == &*changes.at(record.primary),
            "pending metadata stopped borrowing the overlay row");
  }
  writer.sync(table.fullyCached());
  return pending;
}

void syncCommit(const mvd::fs::path &directory, uint64_t txid, size_t count) {
  mvd::FileWriter writer(directory / "mvd.wal");
  writer.append(mvd::encodeCommitRecord(txid, count));
  writer.sync();
}

void checkCached(mvd::Table &table, int64_t primary, int64_t number,
                 const std::string &body, const Storage &storage) {
  require(table.fullyCached() && table.rowCount() == 1,
          "transferred row did not remain cached");
  size_t visited = 0;
  table.forEachCached([&](int64_t key, const mvd::Row &row, bool stable) {
    require(stable && key == primary, "cached row identity changed");
    checkRow(row, primary, number, body);
    storage.check(row);
    ++visited;
    return true;
  });
  require(visited == 1, "cached scan missed the transferred row");
}

void checkOwnership(const mvd::fs::path &directory) {
  mvd::fs::create_directories(directory);
  const int64_t minimum = std::numeric_limits<int64_t>::min();
  const int64_t maximum = std::numeric_limits<int64_t>::max();
  const std::string body = std::string(512, 'x') + '\0' +
      "quoted'\\payload;\xc3\xa9\xe4\xb8\xad";
  const std::string replacement = body + "|replacement";
  {
    mvd::Table first(definition(1), directory / "t1.dat");
    mvd::Table second(definition(2), directory / "t2.dat");
    first.setCacheLimit(8 * 1024 * 1024);
    second.setCacheLimit(8 * 1024 * 1024);
    std::optional<Storage> first_storage, second_storage;
    {
      Changes first_overlay, second_overlay;
      first_overlay.emplace(11, makeRow(11, minimum, body));
      second_overlay.emplace(22, makeRow(22, maximum, body));
      auto &first_row = *first_overlay.at(11);
      auto &second_row = *second_overlay.at(22);
      first_storage.emplace(first_row);
      second_storage.emplace(second_row);
      auto pending = appendAndSync(first, first_overlay, 1);
      const auto other = appendAndSync(second, second_overlay, 1);
      pending.insert(pending.end(), other.begin(), other.end());
      first_storage->check(first_row);
      second_storage->check(second_row);
      syncCommit(directory, 1, pending.size());
      first_storage->check(first_row);
      second_storage->check(second_row);
      first.applyPending(pending);
      require(first_row.empty() && first_row.data() != first_storage->fields,
              "published source still owns the transferred allocation");
      second_storage->check(second_row);
      require(second.rowCount() == 0, "publication consumed another table's row");
      second.applyPending(pending);
      require(second_row.empty() && second_row.data() != second_storage->fields,
              "second publication retained its overlay allocation");
    }
    // These checks run after both overlays and their pending pointers died.
    checkCached(first, 11, minimum, body, *first_storage);
    checkCached(second, 22, maximum, body, *second_storage);

    const auto *directory_before = first.cachedDistanceCandidates(4, nullptr);
    require(directory_before != nullptr && directory_before->size() == 1,
            "replacement fixture did not warm the distance directory");
    mvd::Condition condition;
    condition.column = 1;
    condition.op = mvd::CompareOp::Eq;
    condition.value.setInteger(minimum);
    require(first.equalityCandidates(condition) != nullptr,
            "replacement fixture did not warm the equality directory");
    std::optional<Storage> replacement_storage;
    {
      Changes overlay;
      overlay.emplace(11, makeRow(11, minimum, replacement));
      auto &row = *overlay.at(11);
      replacement_storage.emplace(row);
      auto pending = appendAndSync(first, overlay, 2);
      require(pending[0].op == 2, "replacement was serialized as an insert");
      replacement_storage->check(row);
      syncCommit(directory, 2, pending.size());
      first.applyPending(pending);
      require(row.empty(), "replacement overlay was not consumed");
    }
    checkCached(first, 11, minimum, replacement, *replacement_storage);
    const auto *directory_after = first.cachedDistanceCandidates(4, nullptr);
    require(directory_after != nullptr && directory_after->size() == 1 &&
                (*directory_after)[0].cached != nullptr &&
                (*directory_after)[0].cached->data() == replacement_storage->fields &&
                (*directory_after)[0].vector_data == replacement_storage->vector,
            "rebuilt distance directory borrowed the replaced row");
    const auto *bucket = first.equalityCandidates(condition);
    require(bucket != nullptr && bucket->size() == 1 &&
                (*bucket)[0].cached != nullptr &&
                (*bucket)[0].cached->data() == replacement_storage->fields &&
                (*bucket)[0].vector_data == replacement_storage->vector,
            "rebuilt equality directory borrowed the replaced row");

    first.setCacheLimit(0);
    require(!first.fullyCached(), "zero budget retained cached row storage");
    {
      Changes overlay;
      overlay.emplace(12, makeRow(12, 42, body));
      auto &row = *overlay.at(12);
      const Storage storage(row);
      auto pending = appendAndSync(first, overlay, 3);
      syncCommit(directory, 3, pending.size());
      first.applyPending(pending);
      storage.check(row);
      checkRow(row, 12, 42, body);
    }
    mvd::Row disk;
    require(first.findBase(11, disk), "cache eviction lost the replacement");
    checkRow(disk, 11, minimum, replacement);
    require(first.findBase(12, disk), "uncached publication lost its disk row");
    checkRow(disk, 12, 42, body);
  }
  const auto wal = mvd::scanCommitLog(directory / "mvd.wal");
  require(wal.committed.size() == 3, "durable commits disappeared on restart");
  uint64_t max_seen = 0;
  mvd::Table first(definition(1), directory / "t1.dat");
  mvd::Table second(definition(2), directory / "t2.dat");
  first.load(wal.committed, max_seen);
  second.load(wal.committed, max_seen);
  require(first.rowCount() == 2 && second.rowCount() == 1,
          "restart changed committed row counts");
  mvd::Row recovered;
  require(first.findBase(11, recovered), "restart lost transferred replacement");
  checkRow(recovered, 11, minimum, replacement);
  require(first.findBase(12, recovered), "restart lost uncached publication");
  checkRow(recovered, 12, 42, body);
  require(second.findBase(22, recovered), "restart lost the other table's row");
  checkRow(recovered, 22, maximum, body);
  std::cout << "PASS row allocation transfer, overlay destruction, cache invalidation, "
               "disk publication and all-type durable restart\n";
}

void expectRows(mvd::Database &database, const std::string &table,
                const Rows &expected) {
  const auto result = database.execute(
      "SELECT id,n,body,v FROM " + table + " ORDER BY id");
  require(result && result->select && result->rows == expected,
          "transaction returned incorrect committed or overlay fields");
}

void checkTransactions(const mvd::fs::path &directory) {
  const std::string base(256, 'b'), pending(384, 'p'), committed(512, 'c');
  const Rows before{{"1", "10", base, "[1,2,3,4]"},
                    {"2", "20", "two", "[4,3,2,1]"}};
  const Rows other_before{{"10", "-10", "other", "[0,1,0,1]"}};
  const Rows rolled_back{{"1", "-1", pending, "[-1,0,1,2]"},
                         {"2", "200", "temporary two", "[2,2,2,2]"},
                         {"3", "300", "temporary three", "[3,3,3,3]"}};
  const Rows after{{"1", "-9223372036854775808", committed, "[-4,3,-2,1]"},
                   {"3", "9223372036854775807", "three", "[9,8,7,6]"}};
  const Rows other_after{{"10", "100", committed, "[5,6,7,8]"}};
  {
    mvd::Database database(directory, false);
    for (const char *name : {"t", "u"}) {
      database.execute("CREATE TABLE " + std::string(name) +
          "(id BIGINT PRIMARY KEY,n BIGINT,body VARCHAR(2048),v VECTOR(4))");
    }
    database.execute("INSERT INTO t VALUES(1,10,'" + base +
        "','[1,2,3,4]'),(2,20,'two','[4,3,2,1]')");
    database.execute("INSERT INTO u VALUES(10,-10,'other','[0,1,0,1]')");
    database.execute("BEGIN");
    database.execute("UPDATE t SET body='intermediate' WHERE id=1");
    database.execute("UPDATE t SET n=-1,body='" + pending +
        "',v='[-1,0,1,2]' WHERE id=1");
    database.execute("DELETE FROM t WHERE id=2");
    database.execute("INSERT INTO t VALUES(2,200,'temporary two','[2,2,2,2]'),"
                     "(3,300,'temporary three','[3,3,3,3]')");
    database.execute("UPDATE u SET body='pending other' WHERE id=10");
    expectRows(database, "t", rolled_back);
    bool duplicate = false;
    try {
      database.execute("INSERT INTO t VALUES(4,400,'four','[4,4,4,4]'),"
                       "(1,100,'duplicate','[1,1,1,1]')");
    } catch (const mvd::DBError &error) {
      duplicate = error.code() == "DUPLICATE_PRIMARY_KEY";
    }
    require(duplicate, "duplicate statement did not fail as expected");
    expectRows(database, "t", rolled_back);
    database.execute("ROLLBACK");
    expectRows(database, "t", before);
    expectRows(database, "u", other_before);

    database.execute("BEGIN");
    database.execute("UPDATE t SET body='intermediate again' WHERE id=1");
    database.execute("UPDATE t SET n=-9223372036854775808,body='" + committed +
        "',v='[-4,3,-2,1]' WHERE id=1");
    database.execute("DELETE FROM t WHERE id=2");
    database.execute("INSERT INTO t VALUES(3,0,'temporary','[0,0,0,0]')");
    database.execute("UPDATE t SET n=9223372036854775807,body='three',"
                     "v='[9,8,7,6]' WHERE id=3");
    database.execute("UPDATE u SET n=100,body='" + committed +
        "',v='[5,6,7,8]' WHERE id=10");
    expectRows(database, "t", after);
    expectRows(database, "u", other_after);
    database.execute("COMMIT");
    expectRows(database, "t", after);
    expectRows(database, "u", other_after);
    database.execute("BEGIN");
    database.execute("UPDATE t SET body='discarded after commit' WHERE id=1");
    database.execute("ROLLBACK");
    expectRows(database, "t", after);
  }
  mvd::Database recovered(directory, false);
  expectRows(recovered, "t", after);
  expectRows(recovered, "u", other_after);
  std::cout << "PASS repeated replacement, failed statement, rollback, "
               "multi-table COMMIT and durable restart\n";
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_commit_row_transfer EMPTY_TEMP_DIRECTORY\n";
    return 2;
  }
  const mvd::fs::path root = argv[1];
  if (mvd::fs::exists(root) && !mvd::fs::is_empty(root)) {
    std::cerr << "test directory must be empty\n";
    return 2;
  }
  try {
    checkOwnership(root / "ownership");
    checkTransactions(root / "transactions");
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
