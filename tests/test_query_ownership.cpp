#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
// An odd size avoids counting ordinary row/vector/directory allocations.
constexpr std::size_t long_key_size = 4093;
bool count_long_keys = false;
std::size_t long_key_allocations = 0;
}

void *operator new(std::size_t size) {
  if (count_long_keys && size == long_key_size + 1) {
    ++long_key_allocations;
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

// Exercise the real parser and SELECT implementation in an isolated executable.
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
using Tables = std::unordered_map<std::string, const mvd::TableDef *>;
constexpr std::size_t parser_rows = 384;
constexpr std::size_t condition_pairs = 96;

void require(bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

std::string literal(std::string_view text, char quote = '\'') {
  std::string sql(1, quote);
  for (char c : text) {
    if (c == quote) { sql.append(2, quote); }
    else if (c == '\\') { sql += "\\\\"; }
    else if (c == '\n') { sql += "\\n"; }
    else if (c == '\r') { sql += "\\r"; }
    else if (c == '\t') { sql += "\\t"; }
    else { sql += c; }
  }
  sql += quote;
  return sql;
}

mvd::Statement detached(std::string sql, const Tables &tables) {
  mvd::Statement statement = mvd::EmptyStatement{};
  {
    mvd::Parser parser(sql, tables);
    require(!parser.empty(), "nonempty ownership fixture parsed as empty");
    statement = parser.parse();
    // Poison borrowed input while its allocation is still alive, then destroy
    // both the Parser (including decoded literals) and the SQL buffer.
    std::fill(sql.begin(), sql.end(), '!');
  }
  return statement;
}

void syntaxFailure(const std::string &sql, const Tables &tables) {
  try {
    mvd::Parser parser(sql, tables);
    (void)parser.parse();
  } catch (const mvd::DBError &error) {
    require(error.code() == "SQL_SYNTAX", "malformed literal/comment had wrong error code");
    return;
  }
  throw std::runtime_error("malformed literal/comment was accepted");
}

struct Payload {
  std::string short_text;
  std::string long_text;
  std::vector<float> vector;
};

std::string predicates(const mvd::TableDef &table,
                       const std::vector<Payload> &payloads) {
  std::string sql = " WHERE ";
  for (std::size_t i = 0; i < condition_pairs; ++i) {
    if (i != 0) { sql += " AND "; }
    sql += table.columns[1].name + "=" + literal(payloads[i].short_text);
    sql += " AND " + table.columns[2].name + ">=" +
           literal(payloads[i].long_text, '"');
  }
  return sql;
}

void checkConditions(const std::vector<mvd::Condition> &conditions,
                     const std::vector<Payload> &payloads) {
  require(conditions.size() == 2 * condition_pairs, "predicate count changed");
  for (std::size_t i = 0; i < condition_pairs; ++i) {
    const auto &short_value = conditions[2 * i];
    const auto &long_value = conditions[2 * i + 1];
    require(short_value.column == 1 && short_value.op == mvd::CompareOp::Eq &&
                short_value.value.text() == payloads[i].short_text &&
                long_value.column == 2 && long_value.op == mvd::CompareOp::Ge &&
                long_value.value.text() == payloads[i].long_text,
            "predicate payload borrowed dead parser/input storage");
  }
}

void checkParserOwnership() {
  mvd::TableDef table;
  table.name = "OwnershipTableRetainedAfterParserDestruction";
  table.columns = {{"id", mvd::ColumnType::BigInt, 0},
                   {"short_text", mvd::ColumnType::VarChar, 32},
                   {"LongPayloadColumnRetainedAfterParserDestruction",
                    mvd::ColumnType::VarChar, 2048},
                   {"VectorColumnRetainedAfterParserDestruction",
                    mvd::ColumnType::Vector, 4}};
  table.vector_column = 3;
  const Tables tables{{mvd::lower(table.name), &table}};
  const auto &long_name = table.columns[2].name;
  const auto &vector_name = table.columns[3].name;
  const std::array<std::string_view, 8> short_values{
      "", "O'B", "a\\b", "c\"d", "\n", "\r\t", "'\\\"", "plain"};
  std::vector<Payload> payloads;
  std::string insert_sql =
      "/* ignored ' \" \\\\ */ INSERT INTO " + table.name + "(" + long_name +
      ",id," + vector_name + ",short_text) VALUES \n";
  for (std::size_t i = 0; i < parser_rows; ++i) {
    Payload payload;
    payload.short_text = short_values[i % short_values.size()];
    if (!payload.short_text.empty()) { payload.short_text += std::to_string(i); }
    payload.long_text = std::string(256 + i % 17 * 37, char('a' + i % 26));
    payload.long_text += "|row=" + std::to_string(i);
    if (i % 4 != 0) { payload.long_text += "|'\"\\\n\r\t;--#/*literal*/"; }
    payload.vector = {static_cast<float>(i), -static_cast<float>(i) - 0.5f,
                      0.125f, -2.0f};
    std::string vector_sql = "[" + std::to_string(i) + ",-" +
                             std::to_string(i) + ".5,0.125,-2]";
    if (i % 3 == 1) { vector_sql = literal("\t" + vector_sql + "\n", '"'); }
    if (i % 3 == 2) { vector_sql = literal(vector_sql); }
    if (i != 0) { insert_sql += ","; }
    insert_sql += "(" + literal(payload.long_text, i % 2 ? '\'' : '"') +
                  "," + std::to_string(i) + "," + vector_sql + "," +
                  literal(payload.short_text, i % 2 ? '"' : '\'') + ")";
    if (i % 16 == 0) { insert_sql += " -- ignored unmatched ' and \\\\\n"; }
    payloads.push_back(std::move(payload));
  }
  insert_sql += "; # ignored unmatched \"\n";
  auto insert_statement = detached(std::move(insert_sql), tables);

  const std::string primary_alias = "PrimaryAliasRetainedAfterParserDestruction";
  const std::string distance_alias = "DistanceAliasRetainedAfterParserDestruction";
  std::string select_sql =
      "SELECT id AS " + primary_alias + ",short_text AS s,L2_DISTANCE(" +
      vector_name + "," + literal("[1.25,\t-2.5,0.125,4\n]") +
      ") AS " + distance_alias + ",L2_DISTANCE(" + vector_name +
      ",[5,6,7,8]) AS direct_distance FROM " + table.name + predicates(table, payloads) +
      " ORDER BY " + primary_alias + " DESC,L2_DISTANCE(" + vector_name + "," +
      literal("[9,8,7,6]\t", '"') + ") ASC,id ASC LIMIT 7 OFFSET 3;";
  // Fail during tokenization after many decoded literals have already grown
  // the parser's storage, as well as on small malformed fast/escape paths.
  syntaxFailure(select_sql + " /* unterminated ' \"", tables);
  syntaxFailure(select_sql + " 'unterminated\\", tables);
  auto select_statement = detached(std::move(select_sql), tables);

  const std::string updated_short = "u'\\\t";
  const std::string updated_long = payloads.back().long_text + "'updated\\\n";
  auto update_statement = detached(
      "UPDATE /* ' ignored */ " + table.name + " SET short_text=" +
          literal(updated_short) + "," + long_name + "=" + literal(updated_long, '"') +
          "," + vector_name + "=" + literal("[\n-3.5,4.25,0.125,6\t]") +
          predicates(table, payloads) + "; -- ignored trailing \"\n",
      tables);

  // Reuse short-string objects and long decoded allocation sizes before any
  // assertions, so dangling decoded views cannot pass through untouched bytes.
  std::vector<std::unique_ptr<std::string>> overwritten;
  for (const Payload &payload : payloads) {
    overwritten.push_back(std::make_unique<std::string>(payload.short_text.size(), '~'));
    overwritten.push_back(std::make_unique<std::string>(payload.long_text.size(), '~'));
  }
  const auto &insert = std::get<mvd::InsertStatement>(insert_statement);
  require(insert.table == table.name &&
              insert.columns == std::vector<std::string>{long_name, "id", vector_name,
                                                         "short_text"} &&
              insert.rows.size() == payloads.size(),
          "INSERT names/row count borrowed dead input");
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    const auto &row = insert.rows[i];
    require(row.size() == 4 && row[0].integer() == static_cast<int64_t>(i) &&
                row[1].text() == payloads[i].short_text &&
                row[2].text() == payloads[i].long_text && row[3].vector() == payloads[i].vector,
            "INSERT string/vector payload changed after parser/input destruction");
  }
  const auto &select = std::get<mvd::SelectStatement>(select_statement);
  require(select.table == table.name && select.limit == 7 && select.offset == 3 &&
              select.items.size() == 5 && select.order.size() == 3,
          "SELECT names/shape changed after parser/input destruction");
  require(select.items[0].source_name == "id" && select.items[0].column == 0 &&
              select.items[0].aliased && select.items[0].output_name == primary_alias &&
              select.items[1].source_name == "short_text" &&
              select.items[1].output_name == "s",
          "SELECT projection names borrowed dead SQL storage");
  require(select.items[2].distance && select.items[2].column == 3 &&
              select.items[2].source_name == vector_name &&
              select.items[2].output_name == distance_alias &&
              select.items[2].probe == std::vector<float>{1.25f, -2.5f, 0.125f, 4} &&
              select.items[3].probe == std::vector<float>{5, 6, 7, 8} &&
              select.items[3].output_name == "direct_distance" &&
              select.items[4].hidden && select.items[4].source_name == vector_name &&
              select.items[4].probe == std::vector<float>{9, 8, 7, 6},
          "SELECT projected/hidden vector probes did not own their data");
  require(select.order[0].reference == primary_alias && select.order[0].descending &&
              select.order[1].distance && select.order[1].source_name == vector_name &&
              select.order[1].reference == vector_name && !select.order[1].descending &&
              select.order[1].probe == std::vector<float>{9, 8, 7, 6} &&
              select.order[2].reference == "id",
          "SELECT order names/probes borrowed parser/input storage");
  checkConditions(select.conditions, payloads);
  const auto &update = std::get<mvd::UpdateStatement>(update_statement);
  require(update.table == table.name && update.assignments.size() == 3 &&
              update.assignments[0].column == 1 &&
              update.assignments[0].value.text() == updated_short &&
              update.assignments[1].column == 2 &&
              update.assignments[1].value.text() == updated_long &&
              update.assignments[2].column == 3 &&
              update.assignments[2].value.vector() ==
                  std::vector<float>{-3.5f, 4.25f, 0.125f, 6},
          "UPDATE names/string/vector assignments did not own their data");
  checkConditions(update.conditions, payloads);

  const std::string prefix = "SELECT short_text FROM " + table.name + " WHERE short_text=";
  for (const char *bad : {"'not closed", "\"not closed", "'doubled at end''",
                          "\"doubled at end\"\"", "'trailing backslash\\",
                          "'ok' /* unterminated comment", "'a''b' AND short_text='x\\n"}) {
    syntaxFailure(prefix + bad, tables);
  }
  const std::string comments = "-- ' \" \\\\ ignored\n# unmatched ' ignored\n/* \" ' */";
  mvd::Parser empty(comments, tables);
  require(empty.empty() && empty.firstKeyword().empty(), "comment-only SQL was not empty");
  std::cout << "PASS parser ownership: detached INSERT/SELECT/UPDATE, decoded growth and syntax errors\n";
}

constexpr int cached_rows = 1000;
constexpr int result_limit = 7;
constexpr int result_offset = 3;
constexpr const char *ordered_query =
    "SELECT id FROM cached WHERE id >= 0 ORDER BY ordering_key LIMIT 7 OFFSET 3";

void expectIds(const std::optional<mvd::ExecResult> &result,
               const std::vector<int> &expected) {
  require(result && result->select && !result->spool &&
              result->columns == std::vector<std::string>{"id"} &&
              result->rows.size() == expected.size(), "cached SELECT result shape changed");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    require(result->rows[i] == std::vector<std::string>{std::to_string(expected[i])},
            "VARCHAR ties did not keep ascending primary keys across OFFSET");
  }
}

void measureCachedSelect(mvd::Database &database) {
  const std::vector<int> expected{3, 4, 5, 6, 7, 8, 9};
  expectIds(database.execute(ordered_query), expected);  // Warmup is not counted.
  std::optional<mvd::ExecResult> result;
  long_key_allocations = 0;
  count_long_keys = true;
  try { result = database.execute(ordered_query); }
  catch (...) { count_long_keys = false; throw; }
  count_long_keys = false;
  const std::size_t allocations = long_key_allocations;
  expectIds(result, expected);
  // Permit several copies per retained winner plus fixed overhead, without
  // prescribing an exact allocation count. Copying all 1000 rejected keys in
  // the old scalar heap exceeds this bound by almost an order of magnitude.
  constexpr std::size_t bound = 8 * (result_limit + result_offset) + 32;
  if (allocations > bound) {
    throw std::runtime_error("rejected cached rows allocated long ORDER BY keys: " +
                             std::to_string(allocations) + " > " + std::to_string(bound));
  }
}

void checkCachedSelection(const mvd::fs::path &directory) {
  // The complete ~4 MiB fixture fits even when the caller inherited a tiny
  // cache setting. No other table shares this process-local 64 MiB budget.
#ifdef _WIN32
  require(_putenv_s("MVD_CACHE_LIMIT_MB", "64") == 0, "cannot set fixture cache budget");
#else
  require(::setenv("MVD_CACHE_LIMIT_MB", "64", 1) == 0, "cannot set fixture cache budget");
#endif
  const std::string key(long_key_size, 'K');
  const std::string encoded = literal(key);
  const std::vector<int> base{3, 4, 5, 6, 7, 8, 9};
  const std::vector<int> changed{2, 3, 5, 6, 7, 8, 9};
  {
    mvd::Database database(directory, false);
    database.execute("CREATE TABLE cached(id BIGINT PRIMARY KEY,ordering_key VARCHAR(8192))");
    for (int start = 0; start < cached_rows; start += 64) {
      std::string sql = "INSERT INTO cached VALUES";
      for (int i = start; i < std::min(start + 64, cached_rows); ++i) {
        if (i != start) { sql += ","; }
        // Physical insertion order differs from the ascending primary range.
        sql += "(" + std::to_string(cached_rows - i - 1) + "," + encoded + ")";
      }
      database.execute(sql);
    }
    measureCachedSelect(database);
    // Mutations, rollback and recovery are deliberately outside the counter.
    database.execute("BEGIN");
    database.execute("UPDATE cached SET ordering_key='!' WHERE id=500");
    database.execute("DELETE FROM cached WHERE id=4");
    database.execute("INSERT INTO cached VALUES(-1," + encoded + ")");
    expectIds(database.execute(ordered_query), changed);
    database.execute("ROLLBACK");
    expectIds(database.execute(ordered_query), base);
    database.execute("BEGIN");
    database.execute("UPDATE cached SET ordering_key='!' WHERE id=500");
    database.execute("DELETE FROM cached WHERE id=4");
    database.execute("COMMIT");
    expectIds(database.execute(ordered_query), changed);
  }
  mvd::Database recovered(directory, false);
  expectIds(recovered.execute(ordered_query), changed);
  const auto payload = recovered.execute("SELECT ordering_key FROM cached WHERE id=500");
  require(payload && payload->rows == std::vector<std::vector<std::string>>{{"!"}},
          "committed sort-key update did not survive recovery");
  std::cout << "PASS cached scalar selection: winner-bounded keys, ties, overlay, rollback and restart\n";
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_query_ownership EMPTY_TEMP_DIRECTORY\n";
    return 2;
  }
  try {
    const mvd::fs::path root = argv[1];
    require(!mvd::fs::exists(root) || mvd::fs::is_empty(root), "test directory must be empty");
    checkParserOwnership();
    checkCachedSelection(root / "cached");
  } catch (const std::exception &error) {
    count_long_keys = false;
    std::cerr << "FAIL query ownership: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
