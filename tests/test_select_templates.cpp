#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
bool count_allocations = false;
bool fail_allocation = false;
size_t allocations = 0;
}

void *operator new(std::size_t size) {
  if (fail_allocation) throw std::bad_alloc();
  if (count_allocations) ++allocations;
  if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
using Tables = std::unordered_map<std::string, const mvd::TableDef *>;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

mvd::TableDef definition() {
  mvd::TableDef table;
  table.name = "t";
  table.columns = {{"id", mvd::ColumnType::BigInt, 0},
                   {"tag", mvd::ColumnType::VarChar, 1024},
                   {"v", mvd::ColumnType::Vector, 4}};
  table.vector_column = 2;
  return table;
}

std::string query(std::string_view probe, std::string_view tail =
    " WHERE tag='stable' ORDER BY d,id LIMIT 3 OFFSET 0; /* tail */") {
  return "SELECT id AS identity,L2_DISTANCE(v,'" + std::string(probe) +
      "') AS d FROM t" + std::string(tail);
}

void remember(mvd::SelectTemplateCache &cache, const std::string &sql,
              const Tables &tables) {
  mvd::Parser parser(sql, tables);
  const auto statement = parser.parse();
  cache.remember(sql, parser.singleProbeLiteral(),
                 std::get<mvd::SelectStatement>(statement));
}

std::string errorFor(const std::string &sql, const Tables &tables,
                     mvd::SelectTemplateCache *cache = nullptr) {
  try {
    if (cache && cache->bind(sql)) return {};
    mvd::Parser parser(sql, tables);
    (void)parser.parse();
    return {};
  } catch (const mvd::DBError &error) {
    return error.code() + ":" + error.what();
  }
}

void checkBinding() {
  const auto table = definition();
  const Tables tables{{"t", &table}};
  mvd::SelectTemplateCache cache;
  {
    std::string original = query("[1,2,3,4]");
    remember(cache, original, tables);
    std::fill(original.begin(), original.end(), '!');
  }
  require(cache.size() == 1, "simple query was not retained");
  for (const std::string probe : {
           "[-0,0,1e-45,-1e-45]", "[1e-400,-1e-400,3.4028234e38,1.1754944e-38]",
           "[+1, 2.0, -3e+2,4.]", " \t[1.000000059604644775390625,2,3,4]\n"}) {
    const std::string sql = query(probe);
    const auto *bound = cache.bind(sql);
    require(bound != nullptr, "valid probe missed its retained template");
    mvd::Parser parser(sql, tables);
    const auto statement = parser.parse();
    const auto &expected = std::get<mvd::SelectStatement>(statement);
    const auto &actual = bound->items[1].probe;
    require(actual.size() == expected.items[1].probe.size(), "bound dimension changed");
    for (size_t i = 0; i < actual.size(); ++i) {
      require(std::bit_cast<uint32_t>(actual[i]) ==
                  std::bit_cast<uint32_t>(expected.items[1].probe[i]),
              "bound probe changed float32 bits");
    }
    require(bound->items[0].output_name == "identity" &&
                bound->conditions[0].value.text() == "stable" &&
                bound->limit == 3 && bound->offset == 0,
            "template borrowed destroyed SQL or parser storage");
  }

  const std::string valid = query("[5,6,7,8]");
  for (const std::string probe : {"[]", "[1,2,3]", "[1,2,3,4,5]", "[nan,2,3,4]",
           "[1e999,2,3,4]", "[1e+,2,3,4]", "[1,2,3,4]garbage", "[1,,3,4]"}) {
    const std::string sql = query(probe);
    const auto ordinary_error = errorFor(sql, tables);
    require(!ordinary_error.empty() && errorFor(sql, tables, &cache) == ordinary_error,
            "cache changed invalid-vector error semantics");
    require(cache.bind(valid) != nullptr, "invalid input poisoned retained template");
  }
  for (const std::string probe : {"[1,\\t2,3,4]", "[1,'x',3,4]", "[1,\"x\",3,4]",
                                "[1,2,3,4]' ) AS d FROM t; --"}) {
    const auto sql = query(probe);
    require(cache.bind(sql) == nullptr, "SQL quoting bypassed lexer fallback");
    require(errorFor(sql, tables, &cache) == errorFor(sql, tables),
            "quote fallback changed parser behavior");
  }
  for (const std::pair<std::string, std::string> change : {
      std::pair{"stable", "other"}, {"identity", "other_alias"},
      {"FROM t", "FROM u"}, {"ORDER BY d,id", "ORDER BY id,d"},
      {"LIMIT 3", "LIMIT 2"}, {"OFFSET 0", "OFFSET 1"}, {"tail", "other"},
      {"SELECT", "select"}}) {
    std::string different = valid;
    different.replace(different.find(change.first), change.first.size(), change.second);
    require(cache.bind(different) == nullptr, "different SQL structure reused a plan");
  }

  allocations = 0;
  count_allocations = true;
  for (size_t i = 0; i < 64; ++i) require(cache.bind(valid) != nullptr, "warm bind missed");
  count_allocations = false;
  const size_t cached_allocations = allocations;
  allocations = 0;
  count_allocations = true;
  for (size_t i = 0; i < 64; ++i) {
    mvd::Parser parser(valid, tables);
    (void)parser.parse();
  }
  count_allocations = false;
  require(cached_allocations < allocations, "template reuse did not reduce parsing allocations");
  std::cout << "PASS owned templates, strict probe rebinding, error parity, SQL isolation; "
            << cached_allocations << " versus " << allocations << " parsing allocations\n";
}

void checkBudgetAndFallback() {
  auto table = definition();
  const Tables tables{{"t", &table}};
  mvd::SelectTemplateCache cache;
  const auto original = query("[1,2,3,4]");
  remember(cache, original, tables);
  const auto old_size = cache.size();
  const std::string another = query("[1,2,3,4]", " WHERE tag='another' ORDER BY d LIMIT 1");
  mvd::Parser parser(another, tables);
  const auto statement = parser.parse();
  fail_allocation = true;
  cache.remember(another, parser.singleProbeLiteral(), std::get<mvd::SelectStatement>(statement));
  fail_allocation = false;
  require(cache.size() == old_size && cache.bind(original),
          "optional allocation failure damaged an existing entry");

  for (const std::string sql : {
      "SELECT id,L2_DISTANCE(v,[1,2,3,4]) AS d FROM t ORDER BY d",
      "SELECT id,L2_DISTANCE(v,'[1,\\t2,3,4]') AS d FROM t ORDER BY d",
      "SELECT L2_DISTANCE(v,'[1,2,3,4]'),L2_DISTANCE(v,'[4,3,2,1]') FROM t",
      "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[1,2,3,4]')",
      "SELECT id,L2_DISTANCE(v,'[1,2,3,4]') AS d FROM t ORDER BY L2_DISTANCE(v,'[1,2,3,4]')"}) {
    mvd::SelectTemplateCache separate;
    remember(separate, sql, tables);
    require(separate.size() == 0, "unsupported template shape was retained");
  }
  for (size_t i = 0; i < 40; ++i) {
    const auto sql = query("[1,2,3,4]", " WHERE tag='" + std::to_string(i) +
        "' ORDER BY d LIMIT 1 /*" + std::string(i * 173, 'c') + "*/");
    remember(cache, sql, tables);
    require(cache.size() <= mvd::SelectTemplateCache::kMaxEntries &&
                cache.retainedBytes() <= mvd::SelectTemplateCache::kByteLimit,
            "template cache exceeded its retained-memory bound");
    require(cache.bind(sql) != nullptr, "new bounded entry was not usable");
  }
  remember(cache, original + " /*" + std::string(70000, 'c') + "*/", tables);
  table.columns[2].length = 16384;
  std::string large_probe = "[0";
  for (size_t i = 1; i < 16384; ++i) large_probe += ",0";
  large_probe += ']';
  remember(cache, query(large_probe), tables);
  require(cache.retainedBytes() <= mvd::SelectTemplateCache::kByteLimit,
          "oversized plan bypassed template memory bound");
  cache.clear();
  require(cache.size() == 0 && cache.bind(original) == nullptr &&
              cache.retainedBytes() == sizeof(cache), "cache clear retained owned memory");
  std::cout << "PASS bounded eviction, unsupported shapes, oversized plans, optional allocation failure\n";
}

void expectIds(mvd::Database &database, const std::string &sql,
                std::initializer_list<const char *> expected) {
  const auto result = database.execute(sql);
  require(result && result->select && result->rows.size() == expected.size(),
          "warm SELECT returned incorrect row count");
  size_t index = 0;
  for (const char *id : expected) {
    require(result->rows[index++][0] == id, "warm SELECT returned stale data or incorrect rank");
  }
}

void checkCurrentState(const mvd::fs::path &directory) {
  const std::string zero = query("[0,0,0,0]", " ORDER BY d,id LIMIT 10");
  const std::string nine = query("[9,0,0,0]", " ORDER BY d,id LIMIT 10");
  const std::string filtered = query("[0,0,0,0]", " WHERE tag='a' ORDER BY d,id LIMIT 10");
  {
    mvd::Database database(directory, false);
    database.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,tag VARCHAR(1024),v VECTOR(4))");
    database.execute("INSERT INTO t VALUES(1,'a','[0,0,0,0]'),(2,'b','[8,0,0,0]')");
    expectIds(database, zero, {"1", "2"});
    expectIds(database, nine, {"2", "1"});
    expectIds(database, filtered, {"1"});
    database.execute("BEGIN");
    database.execute("UPDATE t SET v='[10,0,0,0]',tag='b' WHERE id=1");
    expectIds(database, zero, {"2", "1"});
    expectIds(database, filtered, {});
    database.execute("INSERT INTO t VALUES(3,'a','[1,0,0,0]')");
    expectIds(database, zero, {"3", "2", "1"});
    expectIds(database, filtered, {"3"});
    database.execute("DELETE FROM t WHERE id=2");
    expectIds(database, nine, {"1", "3"});
    database.execute("ROLLBACK");
    expectIds(database, zero, {"1", "2"});
    expectIds(database, filtered, {"1"});
    database.execute("BEGIN");
    database.execute("UPDATE t SET v='[10,0,0,0]' WHERE id=1");
    database.execute("INSERT INTO t VALUES(3,'a','[1,0,0,0]')");
    database.execute("DELETE FROM t WHERE id=2");
    database.execute("COMMIT");
    expectIds(database, zero, {"3", "1"});
    expectIds(database, nine, {"1", "3"});
    database.execute("CREATE VECTOR INDEX idx ON t(v)");
    expectIds(database, zero, {"3", "1"});
    database.execute("CREATE TABLE u(id BIGINT PRIMARY KEY,v VECTOR(2))");
    database.execute("INSERT INTO u VALUES(7,'[3,4]')");
    expectIds(database, "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM u ORDER BY d", {"7"});
    expectIds(database, zero, {"3", "1"});
  }
  mvd::Database recovered(directory, false);
  expectIds(recovered, zero, {"3", "1"});
  expectIds(recovered, nine, {"1", "3"});
  expectIds(recovered, filtered, {"3", "1"});
  const auto all = recovered.execute("SELECT id,tag,v FROM t ORDER BY id");
  require(all && all->rows == std::vector<std::vector<std::string>>{
      {"1", "a", "[10,0,0,0]"}, {"3", "a", "[1,0,0,0]"}},
      "restart changed durable fields after warm queries");
  std::cout << "PASS current rows, predicates, transactions, rollback, commit, DDL and durable restart\n";
}
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const mvd::fs::path root = argv[1];
  if (mvd::fs::exists(root) && !mvd::fs::is_empty(root)) return 2;
  try {
    checkBinding();
    checkBudgetAndFallback();
    checkCurrentState(root / "database");
  } catch (const std::exception &error) {
    count_allocations = false;
    fail_allocation = false;
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
