#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
bool count_allocations = false;
bool fail_allocation = false;
std::ptrdiff_t allocations_before_failure = -1;
size_t allocations = 0;
}

void *operator new(std::size_t size) {
  if (fail_allocation || allocations_before_failure == 0) throw std::bad_alloc();
  if (allocations_before_failure > 0) --allocations_before_failure;
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

std::string hiddenQuery(std::string_view probe,
                        std::string_view tail = ",id LIMIT 10",
                        std::string_view predicate = {}) {
  return "SELECT id FROM t" + std::string(predicate) +
      " ORDER BY L2_DISTANCE(v,'" + std::string(probe) + "')" + std::string(tail);
}

void requireProbe(const std::vector<float> &actual,
                  const std::vector<float> &expected) {
  require(actual.size() == expected.size(), "bound probe dimension changed");
  for (size_t i = 0; i < actual.size(); ++i) {
    require(std::bit_cast<uint32_t>(actual[i]) ==
                std::bit_cast<uint32_t>(expected[i]), "bound probe bits changed");
  }
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

void checkUnquotedBinding() {
  const auto table = definition();
  const Tables tables{{"t", &table}};
  for (bool hidden : {false, true}) {
    const auto sqlFor = [&](std::string_view probe) {
      return hidden
          ? "SELECT id FROM t ORDER BY L2_DISTANCE(v," + std::string(probe) + "),id LIMIT 3"
          : "SELECT id,L2_DISTANCE(v," + std::string(probe) + ") AS d FROM t ORDER BY d,id LIMIT 3";
    };
    mvd::SelectTemplateCache cache;
    {
      std::string sql = sqlFor("[1,2,3,4]");
      remember(cache, sql, tables);
      std::fill(sql.begin(), sql.end(), '!');
    }
    require(cache.size() == 1, "unquoted probe template not retained");
    for (const std::string probe : {"[-0,+1.,0.0000003,4]", "[5,6,7,8]"}) {
      const std::string sql = sqlFor(probe);
      const auto *bound = cache.bind(sql);
      require(bound != nullptr, "unquoted probe failed to rebind");
      mvd::Parser parser(sql, tables);
      const auto expected = std::get<mvd::SelectStatement>(parser.parse());
      requireProbe(bound->items[1].probe, expected.items[1].probe);
      if (hidden) requireProbe(bound->order[0].probe, expected.order[0].probe);
    }
    for (const std::string probe : {"[1,--\n2,3,4]", "[1,/* ] */2,3,4]",
             "[1,# ]\n2,3,4]", "'[1,2,3,4]'", "[1,2,3,4] /* comment */",
             "[1e-30,2,3,4]"}) {
      const auto sql = sqlFor(probe);
      require(cache.bind(sql) == nullptr, "unquoted lexical boundary bypassed fallback");
      require(errorFor(sql, tables, &cache) == errorFor(sql, tables),
              "unquoted fallback changed valid SQL/comment behavior");
    }
    for (const std::string probe : {"[]", "[1,2,3]", "[1,,3,4]", "[1e+,2,3,4]",
             "[1e999,2,3,4]", "[1e999,2,3,4e+]", "[1,2,3,4],id",
             "[1,2,3,4]) FROM t; SELECT ("}) {
      const auto *preserved = cache.bind(sqlFor("[5,6,7,8]"));
      require(preserved != nullptr, "unquoted preservation setup missed");
      const auto old_item = preserved->items[1].probe;
      const auto old_order = preserved->order[0].probe;
      const auto old_bytes = cache.retainedBytes();
      const auto sql = sqlFor(probe);
      const auto error = errorFor(sql, tables);
      require(!error.empty() && errorFor(sql, tables, &cache) == error,
              "unquoted template accepted SQL injection or changed errors");
      requireProbe(preserved->items[1].probe, old_item);
      requireProbe(preserved->order[0].probe, old_order);
      require(cache.retainedBytes() == old_bytes,
              "invalid unquoted binding changed cache accounting");
      require(cache.bind(sqlFor("[5,6,7,8]")) != nullptr,
              "invalid unquoted input poisoned template");
    }
  }
  std::cout << "PASS unquoted probes, lexical fallback and SQL isolation\n";
}

void checkHiddenBinding() {
  const auto table = definition();
  const Tables tables{{"t", &table}};
  const auto sqlFor = [](std::string_view probe) {
    return hiddenQuery(probe, " DESC,id DESC LIMIT 3 OFFSET 1; /* tail */",
                       " WHERE tag='stable'");
  };
  mvd::SelectTemplateCache cache;
  {
    std::string sql = sqlFor("[1,2,3,4]");
    remember(cache, sql, tables);
    std::fill(sql.begin(), sql.end(), '!');
  }
  require(cache.size() == 1, "hidden ORDER BY probe was not retained");
  for (const std::string probe : {
           "[-0,0,1e-45,-1e-45]", "[+1, 2.0, -3e+2,4.]",
           "[1e-400,-1e-400,3.4028234e38,1.1754944e-38]",
           " \t[1.0000000596046448,2,3,4]\n"}) {
    const auto sql = sqlFor(probe);
    const auto *bound = cache.bind(sql);
    require(bound != nullptr, "hidden probe binding missed");
    mvd::Parser parser(sql, tables);
    const auto statement = parser.parse();
    const auto &expected = std::get<mvd::SelectStatement>(statement);
    requireProbe(bound->items[1].probe, expected.items[1].probe);
    requireProbe(bound->order[0].probe, expected.order[0].probe);
    require(bound->items[1].hidden && bound->order[0].descending &&
                bound->order[1].descending && bound->limit == 3 &&
                bound->offset == 1 && bound->conditions[0].value.text() == "stable",
            "hidden template changed SQL structure or borrowed input storage");
  }

  const auto valid = sqlFor("[5,6,7,8]");
  const auto next = sqlFor("[9,10,11,12]");
  const auto *bound = cache.bind(valid);
  require(bound != nullptr, "hidden template recovery setup missed");
  const auto old_item = bound->items[1].probe;
  const auto old_order = bound->order[0].probe;
  const size_t old_bytes = cache.retainedBytes();
  const auto unchanged = [&] {
    requireProbe(bound->items[1].probe, old_item);
    requireProbe(bound->order[0].probe, old_order);
    require(cache.retainedBytes() == old_bytes, "failed binding changed accounting");
  };
  for (const std::string probe : {
           "[]", "[1,2,3]", "[1,2,3,4,5]", "[nan,2,3,4]", "[1e999,2,3,4]",
           "[1e+,2,3,4]", "[1,2,3,4]garbage", "[1,,3,4]",
           "[1,2,3,4,1e+]", "[1,2,3,4,1e999]"}) {
    const auto sql = sqlFor(probe);
    const auto error = errorFor(sql, tables);
    require(!error.empty() && errorFor(sql, tables, &cache) == error,
            "hidden binding changed invalid-vector error order");
    unchanged();
    require(cache.bind(valid) != nullptr, "invalid hidden probe poisoned template");
  }
  for (const std::string probe : {
           "[1,\\t2,3,4]", "[1,'x',3,4]", "[1,\"x\",3,4]"}) {
    const auto sql = sqlFor(probe);
    require(cache.bind(sql) == nullptr, "hidden quoting bypassed lexer fallback");
    require(errorFor(sql, tables, &cache) == errorFor(sql, tables),
            "hidden quote fallback changed errors");
    unchanged();
    require(cache.bind(valid) != nullptr, "quote fallback poisoned hidden template");
  }
  for (const std::pair<std::string, std::string> change : {
           std::pair{"stable", "other"}, {"SELECT id", "SELECT tag"},
           {"FROM t", "FROM u"}, {"L2_DISTANCE(v,", "L2_DISTANCE(other,"},
           {"DESC,id DESC", "ASC,id DESC"}, {"id DESC", "id ASC"},
           {"LIMIT 3", "LIMIT 2"}, {"OFFSET 1", "OFFSET 0"}, {"tail", "other"}}) {
    auto sql = valid;
    sql.replace(sql.find(change.first), change.first.size(), change.second);
    require(cache.bind(sql) == nullptr, "different hidden SQL shape reused template");
    unchanged();
  }
  for (std::ptrdiff_t failure : {0, 1}) {
    bool failed = false;
    allocations_before_failure = failure;
    try {
      (void)cache.bind(next);
    } catch (const std::bad_alloc &) {
      failed = true;
    }
    allocations_before_failure = -1;
    require(failed, "probe allocation failure was not exercised");
    unchanged();
    require(cache.bind(next) != nullptr, "allocation failure poisoned hidden template");
    requireProbe(bound->items[1].probe, {9,10,11,12});
    requireProbe(bound->order[0].probe, {9,10,11,12});
    require(cache.bind(valid) != nullptr, "hidden template failed to rebind");
  }

  mvd::SelectTemplateCache later_order;
  remember(later_order,
      "SELECT id FROM t ORDER BY id DESC,L2_DISTANCE(v,'[1,2,3,4]') ASC", tables);
  const auto *later = later_order.bind(
      "SELECT id FROM t ORDER BY id DESC,L2_DISTANCE(v,'[4,3,2,1]') ASC");
  require(later != nullptr && !later->order[0].distance,
          "hidden probe required the first ORDER BY position");
  requireProbe(later->items[1].probe, {4,3,2,1});
  requireProbe(later->order[1].probe, {4,3,2,1});
  std::cout << "PASS hidden probe binding, fallback, shape guards and atomic allocation failure\n";
}

void checkHiddenBudget() {
  auto table = definition();
  const Tables tables{{"t", &table}};
  for (size_t dimension : {1, 4096, 4097, 8192}) {
    table.columns[2].length = static_cast<uint32_t>(dimension);
    std::string probe = "[1";
    for (size_t i = 1; i < dimension; ++i) probe += ",0";
    probe += ']';
    const auto sql = hiddenQuery(probe);
    mvd::SelectTemplateCache cache;
    remember(cache, sql, tables);
    if (dimension == 8192) {
      mvd::SelectTemplateCache visible;
      remember(visible, query(probe), tables);
      require(visible.size() == 1 && cache.size() == 0,
              "two probe buffers bypassed admission budget");
      continue;
    }
    require(cache.size() == 1, "fitting hidden probes were not retained");
    const size_t before = cache.retainedBytes();
    probe[1] = '2';
    const auto *bound = cache.bind(hiddenQuery(probe));
    require(bound != nullptr, "fitting hidden probes failed to bind");
    requireProbe(bound->items[1].probe, bound->order[0].probe);
    const size_t probe_bytes = (bound->items[1].probe.capacity() +
                               bound->order[0].probe.capacity()) * sizeof(float);
    const size_t after = cache.retainedBytes();
    require(after >= sizeof(cache) + probe_bytes &&
                after <= mvd::SelectTemplateCache::kByteLimit,
            "hidden binding did not account for both actual capacities");
    // A >4096-dimensional bind may grow its initial capped reserve. Leave less
    // than that observed growth available and require an unchanged cache miss.
    if (after > before + 128) {
      const size_t growth = after - before;
      // Admission's preliminary measure uses the parser's larger capacities,
      // without sizeof(cache). Keep that measure below its rejection limit.
      const size_t slack = growth - std::min(growth / 2, sizeof(cache) / 2);
      const auto padded = sql + " /*" + std::string(
          mvd::SelectTemplateCache::kByteLimit - before - slack - 5, 'c') + "*/";
      mvd::SelectTemplateCache tight;
      remember(tight, padded, tables);
      require(tight.size() == 1, "budget-pressure template was not admitted");
      const size_t retained = tight.retainedBytes();
      require(tight.bind(padded) == nullptr && tight.size() == 1 &&
                  tight.retainedBytes() == retained,
              "over-budget hidden binding published buffers or changed accounting");
      require(errorFor(padded, tables, &tight).empty(),
              "over-budget hidden binding prevented ordinary parsing");
    }
    cache.clear();
    require(cache.retainedBytes() == sizeof(cache) && cache.size() == 0,
            "clear retained hidden probe buffers");
  }
  std::cout << "PASS dual-probe admission, actual capacities, budget fallback and clear\n";
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
      "SELECT id,L2_DISTANCE(v,[1,/* comment */2,3,4]) AS d FROM t ORDER BY d",
      "SELECT id,L2_DISTANCE(v,'[1,\\t2,3,4]') AS d FROM t ORDER BY d",
      "SELECT L2_DISTANCE(v,'[1,2,3,4]'),L2_DISTANCE(v,'[4,3,2,1]') FROM t",
      "SELECT id FROM t ORDER BY L2_DISTANCE(v,[1,--\n2,3,4])",
      "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[1,\\t2,3,4]')",
      "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[1,2,3,4]'),L2_DISTANCE(v,'[1,2,3,4]')",
      "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[1,2,3,4]'),L2_DISTANCE(v,'[4,3,2,1]')",
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

void checkCurrentState(const mvd::fs::path &directory, bool hidden = false,
                       bool unquoted = false) {
  std::string zero = hidden ? hiddenQuery("[0,0,0,0]")
      : query("[0,0,0,0]", " ORDER BY d,id LIMIT 10");
  std::string nine = hidden ? hiddenQuery("[9,0,0,0]")
      : query("[9,0,0,0]", " ORDER BY d,id LIMIT 10");
  std::string filtered = hidden
      ? hiddenQuery("[0,0,0,0]", ",id LIMIT 10", " WHERE tag='a'")
      : query("[0,0,0,0]", " WHERE tag='a' ORDER BY d,id LIMIT 10");
  if (unquoted) {
    for (std::string *sql : {&zero, &nine, &filtered}) {
      sql->erase(sql->find("'["), 1);
      sql->erase(sql->find("]'") + 1, 1);
    }
  }
  {
    mvd::Database database(directory, false);
    database.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,tag VARCHAR(1024),v VECTOR(4))");
    database.execute("INSERT INTO t VALUES(1,'a','[0,0,0,0]'),(2,'b','[8,0,0,0]')");
    expectIds(database, zero, {"1", "2"});
    expectIds(database, nine, {"2", "1"});
    expectIds(database, filtered, {"1"});
    if (hidden) {
      const auto result = database.execute(zero);
      require(result && result->columns == std::vector<std::string>{"id"} &&
                  result->rows.size() == 2 &&
                  result->rows[0].size() == 1, "hidden distance leaked into output");
      const auto descending = " DESC,id ASC LIMIT 1 OFFSET 1";
      expectIds(database, hiddenQuery("[0,0,0,0]", descending), {"1"});
      expectIds(database, hiddenQuery("[9,0,0,0]", descending), {"2"});
      expectIds(database, hiddenQuery("[4,0,0,0]", descending), {"2"});
      const auto pk_descending = " ASC,id DESC LIMIT 1";
      expectIds(database, hiddenQuery("[5,0,0,0]", pk_descending), {"2"});
      expectIds(database, hiddenQuery("[4,0,0,0]", pk_descending), {"2"});
      expectIds(database, hiddenQuery("[0,0,0,0]", pk_descending), {"1"});
    }
    database.execute("BEGIN");
    database.execute("UPDATE t SET v='[10,0,0,0]',tag='b' WHERE id=1");
    if (hidden) {
      bool failed = false;
      try {
        database.execute(hiddenQuery("[1,2,3,4,1e+]"));
      } catch (const mvd::DBError &error) {
        failed = error.code() == "SQL_SYNTAX";
      }
      require(failed, "failed hidden binding changed transaction error semantics");
    }
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
    checkUnquotedBinding();
    checkHiddenBinding();
    checkHiddenBudget();
    checkBudgetAndFallback();
    checkCurrentState(root / "database");
    checkCurrentState(root / "hidden-database", true);
    checkCurrentState(root / "unquoted-database", false, true);
    checkCurrentState(root / "unquoted-hidden-database", true, true);
  } catch (const std::exception &error) {
    count_allocations = false;
    fail_allocation = false;
    allocations_before_failure = -1;
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
