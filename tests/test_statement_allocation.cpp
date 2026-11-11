#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
long long allocation_budget = -1;
std::size_t allocated_bytes = 0;
std::size_t peak_allocated_bytes = 0;
std::size_t allocation_byte_limit = std::numeric_limits<std::size_t>::max();
struct alignas(std::max_align_t) AllocationHeader {
  std::size_t bytes;
};
}

void *operator new(std::size_t size) {
  if (allocation_budget == 0) {
    allocation_budget = -1;
    throw std::bad_alloc();
  }
  if (allocation_budget > 0) {
    --allocation_budget;
  }
  size = size == 0 ? 1 : size;
  if (allocated_bytes > allocation_byte_limit ||
      size > allocation_byte_limit - allocated_bytes ||
      size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(sizeof(AllocationHeader) + size)) {
    auto *header = static_cast<AllocationHeader *>(memory);
    header->bytes = size;
    allocated_bytes += size;
    if (peak_allocated_bytes < allocated_bytes) {
      peak_allocated_bytes = allocated_bytes;
    }
    return header + 1;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept {
  if (memory != nullptr) {
    auto *header = static_cast<AllocationHeader *>(memory) - 1;
    allocated_bytes -= header->bytes;
    std::free(header);
  }
}
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }

// Include the real implementation; only this isolated executable replaces new.
#define MVD_TEST_FAULTS
#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
using Snapshot = std::vector<std::vector<std::string>>;

Snapshot snapshot(mvd::Database &database) {
  auto result = database.execute("SELECT id,s FROM t ORDER BY id");
  if (!result || !result->select) {
    throw std::runtime_error("snapshot did not return rows");
  }
  return result->rows;
}

void checkCase(const mvd::fs::path &directory, const std::string &label,
               const std::string &statement) {
  {
    mvd::Database database(directory, false);
    database.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64))");
    database.execute("INSERT INTO t VALUES(1,'one'),(2,'two'),(3,'three')");
  }
  std::size_t failures = 0;
  for (long long index = 0; index < 2048; ++index) {
    Snapshot before;
    Snapshot expected_after_resolution;
    bool failed = false;
    {
      mvd::Database database(directory, false);
      const Snapshot committed = snapshot(database);
      database.execute("BEGIN");
      database.execute("UPDATE t SET s='prior-" + std::to_string(index) +
                       "' WHERE id=1");
      before = snapshot(database);
      allocation_budget = index;
      try {
        database.execute(statement);
      } catch (const std::bad_alloc &) {
        failed = true;
      } catch (...) {
        allocation_budget = -1;
        throw;
      }
      allocation_budget = -1;
      if (!failed) {
        Snapshot expected = before;
        if (label == "INSERT") {
          expected.push_back({"4", "four"});
          expected.push_back({"5", "five"});
          expected.push_back({"6", "six"});
        } else if (label == "UPDATE") {
          for (auto &row : expected) {
            row[1] = "a substantially longer replacement value";
          }
        } else if (label == "DELETE") {
          expected.clear();
        } else {
          throw std::runtime_error("unrecognized allocation fixture");
        }
        if (snapshot(database) != expected) {
          throw std::runtime_error("successful statement returned incorrect state");
        }
        database.execute("ROLLBACK");
        if (snapshot(database) != committed) {
          throw std::runtime_error("successful statement did not roll back");
        }
        std::cout << "PASS " << label << ": " << failures
                  << " allocation failures; COMMIT/ROLLBACK/restart and success verified\n";
        return;
      }
      ++failures;
      const Snapshot after = snapshot(database);
      if (after != before) {
        std::cerr << label << " allocation failure #" << index
                  << " changed statement-visible state: expected " << before.size()
                  << " rows, got " << after.size() << " rows\n";
        for (const auto &row : after) {
          std::cerr << "  actual " << row[0] << ' ' << row[1] << '\n';
        }
        throw std::runtime_error("failed statement was not atomic");
      }
      if (index % 2 == 0) {
        database.execute("COMMIT");
        expected_after_resolution = before;
      } else {
        database.execute("ROLLBACK");
        expected_after_resolution = committed;
      }
    }
    mvd::Database recovered(directory, false);
    if (snapshot(recovered) != expected_after_resolution) {
      throw std::runtime_error("transaction resolution did not survive restart");
    }
  }
  throw std::runtime_error("allocation enumeration never reached successful execution");
}

void environment(const char *name, const char *value) {
#ifdef _WIN32
  if (_putenv_s(name, value) != 0) {
    throw std::runtime_error("cannot configure test environment");
  }
#else
  const int result = *value == '\0' ? unsetenv(name) : setenv(name, value, 1);
  if (result != 0) {
    throw std::runtime_error("cannot configure test environment");
  }
#endif
}

void spilledIoFailures(const mvd::fs::path &directory) {
  environment("MVD_TRANSACTION_MEMORY_MB", "0");
  {
    mvd::Database database(directory, false);
    database.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64))");
    database.execute("INSERT INTO t VALUES(1,'one'),(2,'two'),(3,'three')");
    for (const char *hook : {"MVD_TEST_FAIL_TRANSACTION_WRITE",
                             "MVD_TEST_FAIL_TRANSACTION_READ"}) {
      database.execute("BEGIN");
      database.execute("UPDATE t SET s='prior' WHERE id=1");
      const Snapshot expected = snapshot(database);
      environment(hook, "1");
      bool failed = false;
      try {
        database.execute("UPDATE t SET s='failed-statement'");
      } catch (const mvd::DBError &error) {
        failed = error.code() == "IO_ERROR";
      }
      environment(hook, "");
      if (!failed || snapshot(database) != expected) {
        throw std::runtime_error("transaction scratch I/O failure changed visible rows");
      }
      database.execute("COMMIT");
      if (snapshot(database) != expected) {
        throw std::runtime_error("prior work did not commit after scratch I/O failure");
      }
    }
    database.execute("BEGIN");
    database.execute("UPDATE t SET s='unconfirmed'");
    environment("MVD_TEST_FAIL_TRANSACTION_READ", "1");
    bool fatal = false;
    try {
      database.execute("COMMIT");
    } catch (const mvd::RecoveryRequired &) {
      fatal = true;
    }
    environment("MVD_TEST_FAIL_TRANSACTION_READ", "");
    if (!fatal) {
      throw std::runtime_error("commit scratch failure was not fail-stop");
    }
  }
  mvd::Database recovered(directory, false);
  if (snapshot(recovered) != Snapshot{{"1", "prior"}, {"2", "two"}, {"3", "three"}}) {
    throw std::runtime_error("scratch commit read failure damaged committed rows");
  }
  std::cout << "PASS scratch write/read failures preserve prior statements and durable rows\n";
}

void boundedTransactions(const mvd::fs::path &directory) {
  environment("MVD_CACHE_LIMIT_MB", "1");
  environment("MVD_TRANSACTION_MEMORY_MB", "1");
  constexpr std::size_t rows = 4096;
  constexpr std::size_t dimensions = 1024;
  std::string tail;
  for (std::size_t i = 1; i < dimensions; ++i) {
    tail += ",0";
  }
  tail += "]";
  const auto insert = [&](mvd::Database &database, std::size_t begin,
                          std::size_t end) {
    for (std::size_t first = begin; first < end; first += 32) {
      std::string sql = "INSERT INTO t VALUES";
      for (std::size_t id = first; id < std::min(first + 32, end); ++id) {
        if (id != first) { sql += ","; }
        sql += "(" + std::to_string(id) + ",0,'[" + std::to_string(id) + tail + "')";
      }
      database.execute(sql);
    }
  };
  {
    mvd::Database database(directory, false);
    database.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,tag BIGINT,v VECTOR(1024))");
    insert(database, 1, rows + 1);
    const auto initial_bytes = allocated_bytes;
    peak_allocated_bytes = allocated_bytes;
    allocation_byte_limit = initial_bytes + 6 * 1024 * 1024;
    try {
      const auto updated = database.execute("UPDATE t SET tag=7");
      if (!updated || updated->affected != rows) {
        throw std::runtime_error("bounded UPDATE affected the wrong row count");
      }
      database.execute("BEGIN");
      database.execute("UPDATE t SET tag=9");
      database.execute("UPDATE t SET v='[0" + tail + "' WHERE id>=1");
      auto found = database.execute("SELECT id,tag FROM t ORDER BY id DESC LIMIT 1");
      if (found->rows != Snapshot{{std::to_string(rows), "9"}}) {
        throw std::runtime_error("bounded transaction did not read its UPDATE");
      }
      database.execute("ROLLBACK");
      database.execute("BEGIN");
      insert(database, rows + 1, 2 * rows + 1);
      found = database.execute("SELECT id FROM t ORDER BY id DESC LIMIT 1");
      if (found->rows != Snapshot{{std::to_string(2 * rows)}}) {
        throw std::runtime_error("bounded transaction did not read its INSERT");
      }
      database.execute("ROLLBACK");
      database.execute("BEGIN");
      database.execute("UPDATE t SET tag=11");
      database.execute("COMMIT");
      std::cout << "PASS vector payload=" << rows * dimensions * sizeof(float)
                << " bytes; UPDATE, long INSERT, COMMIT and ROLLBACK under additional 6 MiB heap cap; "
                << "measured peak delta=" << peak_allocated_bytes - initial_bytes << " bytes\n";
    } catch (...) {
      allocation_byte_limit = std::numeric_limits<std::size_t>::max();
      throw;
    }
    allocation_byte_limit = std::numeric_limits<std::size_t>::max();
  }
  mvd::Database recovered(directory, false);
  const auto found = recovered.execute("SELECT id,tag FROM t ORDER BY id DESC LIMIT 1");
  if (found->rows != Snapshot{{std::to_string(rows), "11"}}) {
    throw std::runtime_error("bounded committed UPDATE did not survive restart");
  }
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_statement_allocation EMPTY_TEMP_DIRECTORY\n";
    return 2;
  }
  const mvd::fs::path root = argv[1];
  if (mvd::fs::exists(root) && !mvd::fs::is_empty(root)) {
    std::cerr << "test directory must be empty\n";
    return 2;
  }
  try {
    checkCase(root / "insert", "INSERT",
              "INSERT INTO t VALUES(4,'four'),(5,'five'),(6,'six')");
    checkCase(root / "update", "UPDATE",
              "UPDATE t SET s='a substantially longer replacement value' WHERE id>=1");
    checkCase(root / "delete", "DELETE", "DELETE FROM t WHERE id>=1");
    environment("MVD_TRANSACTION_MEMORY_MB", "0");
    checkCase(root / "spill-insert", "INSERT",
              "INSERT INTO t VALUES(4,'four'),(5,'five'),(6,'six')");
    checkCase(root / "spill-update", "UPDATE",
              "UPDATE t SET s='a substantially longer replacement value' WHERE id>=1");
    checkCase(root / "spill-delete", "DELETE", "DELETE FROM t WHERE id>=1");
    spilledIoFailures(root / "spill-io");
    boundedTransactions(root / "bounded");
  } catch (const std::exception &error) {
    allocation_budget = -1;
    allocation_byte_limit = std::numeric_limits<std::size_t>::max();
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
