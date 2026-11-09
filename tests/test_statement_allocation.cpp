#include <cstdlib>
#include <new>

namespace {
long long allocation_budget = -1;
}

void *operator new(std::size_t size) {
  if (allocation_budget == 0) {
    allocation_budget = -1;
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

// Include the real implementation; only this isolated executable replaces new.
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
  } catch (const std::exception &error) {
    allocation_budget = -1;
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
