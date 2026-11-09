#include <cstdlib>
#include <iterator>
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

// Only this standalone test executable replaces global allocation.
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
static_assert(std::is_nothrow_move_assignable_v<mvd::TableDef>);
static_assert(std::is_nothrow_move_constructible_v<mvd::ExecResult>);

using Bytes = std::vector<uint8_t>;
using Rows = std::vector<std::vector<std::string>>;

Bytes catalogBytes(const mvd::fs::path &directory) {
  const auto path = directory / "catalog.bin";
  if (!mvd::fs::exists(path)) {
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read test catalog");
  }
  return Bytes(std::istreambuf_iterator<char>(input),
               std::istreambuf_iterator<char>());
}

bool selectTable(mvd::Database &database, Rows *rows = nullptr) {
  try {
    const auto result = database.execute("SELECT id,s FROM t ORDER BY id");
    if (!result || !result->select) {
      throw std::runtime_error("SELECT did not return a result");
    }
    if (rows != nullptr) {
      *rows = result->rows;
    }
    return true;
  } catch (const mvd::DBError &error) {
    if (error.code() != "UNKNOWN_TABLE") {
      throw;
    }
    return false;
  }
}

void requireIndexedCatalog(const Bytes &bytes) {
  size_t offset = 0;
  size_t records = 0;
  mvd::TableDef last;
  while (offset < bytes.size()) {
    size_t pos = offset;
    if (mvd::readScalar<uint32_t>(bytes, pos) != mvd::kCatalogMagic ||
        mvd::readScalar<uint16_t>(bytes, pos) != mvd::kFormatVersion) {
      throw std::runtime_error("invalid catalog record");
    }
    pos = offset + 8;
    const auto length = mvd::readScalar<uint64_t>(bytes, pos);
    const auto checksum = mvd::readScalar<uint32_t>(bytes, pos);
    const size_t start = offset + 24;
    if (start > bytes.size() || length > bytes.size() - start) {
      throw std::runtime_error("incomplete catalog record");
    }
    Bytes payload(bytes.begin() + start, bytes.begin() + start + length);
    if (mvd::crc32(payload) != checksum) {
      throw std::runtime_error("catalog checksum mismatch");
    }
    last = mvd::deserializeTableDefinition(payload);
    offset = start + static_cast<size_t>(length);
    ++records;
  }
  if (records != 2 || last.name != "t" || !last.vector_index) {
    throw std::runtime_error("successful index was not recorded");
  }
}

struct Counts {
  size_t failures = 0;
  size_t violations = 0;
};

Counts enumerate(const mvd::fs::path &root, bool index_case) {
  const std::string label = index_case ? "CREATE_INDEX" : "CREATE_TABLE";
  const std::string statement = index_case
      ? "CREATE VECTOR INDEX ix ON t(v)"
      : "CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64))";
  const Rows seed{{"1", "one"}, {"2", "two"}};
  Counts counts;
  for (long long budget = 0; budget < 2048; ++budget) {
    const auto directory = root / (label + "-" + std::to_string(budget));
    Bytes before;
    Bytes after;
    bool failed = false;
    bool injected = false;
    bool live_visible = false;
    bool restart_visible = false;
    {
      mvd::Database database(directory, false);
      if (index_case) {
        database.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64),v VECTOR(2))");
        database.execute("INSERT INTO t VALUES(1,'one','[1,2]'),(2,'two','[3,4]')");
      }
      if (selectTable(database) != index_case) {
        throw std::runtime_error("incorrect initial table visibility");
      }
      before = catalogBytes(directory);
      allocation_injected = false;
      allocation_budget = budget;
      try {
        database.execute(statement);
      } catch (const std::bad_alloc &) {
        failed = true;
      } catch (...) {
        allocation_budget = -1;
        throw;
      }
      allocation_budget = -1;
      injected = allocation_injected;
      if (injected != failed) {
        throw std::runtime_error("injected allocation failure was swallowed");
      }
      after = catalogBytes(directory);
      Rows rows;
      live_visible = selectTable(database, &rows);
      if (index_case && (!live_visible || rows != seed)) {
        throw std::runtime_error("index attempt changed existing rows");
      }
    }
    {
      mvd::Database recovered(directory, false);
      Rows rows;
      restart_visible = selectTable(recovered, &rows);
      if (index_case && (!restart_visible || rows != seed)) {
        throw std::runtime_error("index restart changed existing rows");
      }
      if (catalogBytes(directory) != after) {
        throw std::runtime_error("restart changed the captured catalog bytes");
      }
    }
    if (failed) {
      ++counts.failures;
      const bool violated = before != after ||
          (!index_case && (live_visible || restart_visible));
      if (violated) {
        ++counts.violations;
        std::cout << "COUNTEREXAMPLE " << label << " budget=" << budget
                  << " exception=bad_alloc catalog_bytes=" << before.size()
                  << "->" << after.size()
                  << " live_table=" << (live_visible ? "present" : "UNKNOWN_TABLE")
                  << " restart_table=" << (restart_visible ? "present" : "UNKNOWN_TABLE")
                  << " directory=" << directory.string() << std::endl;
      }
    } else {
      if (injected || !live_visible || !restart_visible || before == after) {
        throw std::runtime_error("successful DDL did not survive restart");
      }
      if (index_case) {
        requireIndexedCatalog(after);
      }
      std::cout << "SUMMARY " << label << " failures=" << counts.failures
                << " violations=" << counts.violations
                << " first_success_budget=" << budget << std::endl;
      return counts;
    }
  }
  throw std::runtime_error("allocation enumeration never reached success");
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_catalog_allocation EMPTY_TEMP_DIRECTORY\n";
    return 2;
  }
  try {
    const mvd::fs::path root = argv[1];
    if (mvd::fs::exists(root) && !mvd::fs::is_empty(root)) {
      throw std::runtime_error("test directory must be empty");
    }
    const Counts table = enumerate(root, false);
    const Counts index = enumerate(root, true);
    return table.violations + index.violations == 0 ? 0 : 1;
  } catch (const std::exception &error) {
    allocation_budget = -1;
    std::cerr << "HARNESS_ERROR " << error.what() << '\n';
    return 2;
  }
}
