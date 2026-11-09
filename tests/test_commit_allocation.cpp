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

#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
struct Snapshot {
  std::vector<std::vector<std::string>> first;
  std::vector<std::vector<std::string>> second;
  bool operator==(const Snapshot &) const = default;
};
Snapshot snapshot(mvd::Database &database, bool multiple_tables) {
  Snapshot result{database.execute("SELECT id,s FROM t ORDER BY id")->rows, {}};
  if (multiple_tables) {
    result.second = database.execute("SELECT id,s FROM u ORDER BY id")->rows;
  }
  return result;
}

void enumerate(const mvd::fs::path &root, bool multiple_tables, bool rollback_retry) {
  std::size_t retryable = 0, recovery_required = 0;
  for (long long index = 0; index < 512; ++index) {
    const auto directory = root /
        (std::to_string(multiple_tables) + "-" + std::to_string(rollback_retry)) /
        std::to_string(index);
    auto database = std::make_unique<mvd::Database>(directory, false);
    database->execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(32))");
    database->execute("INSERT INTO t VALUES(1,'base'),(2,'two')");
    if (multiple_tables) {
      database->execute("CREATE TABLE u(id BIGINT PRIMARY KEY,s VARCHAR(32))");
      database->execute("INSERT INTO u VALUES(10,'other-base')");
    }
    const Snapshot committed = snapshot(*database, multiple_tables);
    database->execute("BEGIN");
    database->execute("UPDATE t SET s='pending' WHERE id=1");
    database->execute("INSERT INTO t VALUES(3,'three')");
    if (multiple_tables) {
      database->execute("UPDATE u SET s='other-pending' WHERE id=10");
      database->execute("INSERT INTO u VALUES(11,'eleven')");
    }
    const Snapshot pending = snapshot(*database, multiple_tables);
    bool retry = false, fatal = false;
    allocation_budget = index;
    try {
      database->execute("COMMIT");
    } catch (const std::bad_alloc &) {
      retry = true;
    } catch (const mvd::RecoveryRequired &) {
      fatal = true;
    } catch (...) {
      allocation_budget = -1;
      throw;
    }
    allocation_budget = -1;
    if (retry) {
      ++retryable;
      if (snapshot(*database, multiple_tables) != pending) {
        std::cerr << "COMMIT allocation failure #" << index
                  << " discarded pending work before returning a retryable error\n";
        throw std::runtime_error("retryable COMMIT failure lost prior successful work");
      }
      database->execute(rollback_retry ? "ROLLBACK" : "COMMIT");
      if (snapshot(*database, multiple_tables) !=
          (rollback_retry ? committed : pending)) {
        throw std::runtime_error("retry or rollback produced incorrect rows");
      }
    } else if (fatal) {
      ++recovery_required;
      // Never execute another request against a recovery-required instance.
    } else if (snapshot(*database, multiple_tables) != pending) {
      throw std::runtime_error("successful COMMIT changed the expected rows");
    }
    database.reset();
    mvd::Database recovered(directory, false);
    const Snapshot durable = snapshot(recovered, multiple_tables);
    const Snapshot &expected = retry && rollback_retry ? committed : pending;
    if (fatal ? durable != committed && durable != pending : durable != expected) {
      throw std::runtime_error("COMMIT reload did not produce an allowed complete state");
    }
    if (!retry && !fatal) {
      std::cout << "PASS COMMIT tables=" << (multiple_tables ? 2 : 1)
                << " retry=" << (rollback_retry ? "ROLLBACK" : "COMMIT")
                << ": " << retryable << " retryable failures, "
                << recovery_required
                << " recovery-required failures; retry and reload verified\n";
      return;
    }
  }
  throw std::runtime_error("COMMIT allocation enumeration never reached success");
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_commit_allocation EMPTY_TEMP_DIRECTORY\n";
    return 2;
  }
  const mvd::fs::path root = argv[1];
  if (mvd::fs::exists(root) && !mvd::fs::is_empty(root)) {
    std::cerr << "test directory must be empty\n";
    return 2;
  }
  try {
    for (const bool multiple_tables : {false, true}) {
      for (const bool rollback_retry : {false, true}) {
        enumerate(root, multiple_tables, rollback_retry);
      }
    }
    return 0;
  } catch (const std::exception &error) {
    allocation_budget = -1;
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
