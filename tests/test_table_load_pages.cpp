// Linux syscall audit: advice is optional; recovery and file bytes are not.
#ifndef __linux__
#include <iostream>
int main() { std::cout << "SKIP Linux table-load page audit\n"; }
#else
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace audit {
bool active = false;
bool invalid = false;
size_t calls = 0, syncs = 0;
uint64_t released = 0, readable = 0;
ino_t inode = 0;
long page = 4096;
int error = 0;
constexpr int preserved_errno = EDOM;
}

extern "C" int __real_fsync(int);
extern "C" long __real_sysconf(int);
extern "C" int __real_posix_fadvise(int, off_t, off_t, int);
extern "C" int __wrap_fsync(int descriptor) {
  if (audit::active) ++audit::syncs;
  return __real_fsync(descriptor);
}
extern "C" long __wrap_sysconf(int name) {
  if (!audit::active || name != _SC_PAGESIZE) return __real_sysconf(name);
  errno = ERANGE;
  return audit::page;
}
extern "C" int __wrap_posix_fadvise(int descriptor, off_t begin,
                                    off_t length, int advice) {
  if (!audit::active) return __real_posix_fadvise(descriptor, begin, length, advice);
  struct stat file{};
  ++audit::calls;
  const bool valid = fstat(descriptor, &file) == 0;
  audit::invalid |= !valid || file.st_ino != audit::inode ||
      advice != POSIX_FADV_DONTNEED || audit::page <= 0 || begin < 0 ||
      length <= 0 || length > 4 * 1024 * 1024 ||
      static_cast<uint64_t>(begin) != audit::released;
  if (valid && audit::page > 0 && begin >= 0 && length > 0) {
    audit::invalid |= begin % audit::page != 0 || length % audit::page != 0 ||
        static_cast<uint64_t>(begin + length) > audit::readable ||
        begin + length > file.st_size;
  }
  if (audit::error == 0) audit::released += static_cast<uint64_t>(length);
  errno = EBUSY;
  return audit::error;
}

#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
std::string bytes(const mvd::fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void start(const mvd::fs::path &path, uint64_t readable = 0) {
  struct stat file{};
  require(stat(path.c_str(), &file) == 0, "cannot stat fixture");
  audit::active = true;
  audit::invalid = false;
  audit::calls = audit::syncs = 0;
  audit::released = 0;
  audit::readable = readable;
  audit::inode = file.st_ino;
  audit::page = __real_sysconf(_SC_PAGESIZE);
  audit::error = 0;
}
void stop() {
  audit::active = false;
  require(!audit::invalid && audit::syncs == 0,
          "load advice used invalid ranges or introduced a sync");
}

void checkReader(const mvd::fs::path &root) {
  const auto path = root / "reader.dat";
  const std::string expected(12 * 1024 * 1024 + 8193, 'x');
  { std::ofstream file(path, std::ios::binary); file << expected; }
  for (int mode = 0; mode != 5; ++mode) {
    start(path);
    if (mode == 2) audit::error = ENOSYS;
    if (mode == 3) audit::page = 0;
    if (mode == 4) audit::page = 65537;
    {
      mvd::TableLoadReader reader(path, mode != 1);
      require(static_cast<bool>(reader), "cannot open reader");
      std::array<char, 3001> buffer{};
      size_t consumed = 0;
      while (consumed != expected.size()) {
        const size_t count = std::min(buffer.size(), expected.size() - consumed);
        require(reader.read(buffer.data(), count), "reader truncated bytes");
        require(std::string_view(buffer.data(), count) ==
                    std::string_view(expected).substr(consumed, count),
                "reader changed decoded bytes");
        consumed += count;
        audit::readable = consumed;
        errno = audit::preserved_errno;
        reader.releaseConsumed(consumed);
        require(errno == audit::preserved_errno, "advice changed errno");
      }
      errno = audit::preserved_errno;
      reader.releaseConsumed(consumed, true);
      require(errno == audit::preserved_errno, "final advice changed errno");
      require(!reader.read(buffer.data(), 1), "reader did not detect EOF");
    }
    stop();
    if (mode == 0) {
      require(audit::calls > 1 && audit::released ==
                  expected.size() / audit::page * audit::page,
              "reader did not release exactly the consumed complete pages");
    } else {
      require(audit::calls == (mode == 2 ? 1U : 0U),
              "disabled or unsupported advice was retried");
    }
    require(bytes(path) == expected, "reader advice changed file contents");
  }
  // A real stdio error must remain an IO_ERROR, distinct from ordinary EOF.
  mvd::TableLoadReader directory(root, false);
  require(static_cast<bool>(directory), "cannot open directory error fixture");
  char byte;
  bool failed = false;
  try { directory.read(&byte, 1); }
  catch (const mvd::DBError &error) { failed = error.code() == "IO_ERROR"; }
  require(failed, "reader suppressed an I/O error");
}

void checkRecovery(const mvd::fs::path &root) {
  const std::string text(2048, 'q');
  {
    mvd::Database database(root, false);
    database.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(4096))");
    std::string insert = "INSERT INTO t VALUES";
    for (int key = 0; key != 150; ++key) {
      if (key != 0) insert += ',';
      insert += '(' + std::to_string(key) + ",'" + text + "')";
    }
    database.execute(insert);
  }
  const auto path = root / "t1.dat";
  const auto original = bytes(path);
  const auto wal_bytes = bytes(root / "mvd.wal");
  const auto catalog_bytes = bytes(root / "catalog.bin");
  const auto wal = mvd::scanCommitLog(root / "mvd.wal");
  mvd::TableDef definition;
  definition.oid = 1;
  definition.name = "t";
  definition.columns = {{"id", mvd::ColumnType::BigInt, 0},
                        {"s", mvd::ColumnType::VarChar, 4096}};
  for (int mode = 0; mode != 4; ++mode) {
    mvd::Table table(definition, path);
    const size_t limit = mode == 2 ? original.size() * 2 - 1
        : mode == 3 ? 1 : 2 * 1024 * 1024;
    table.setCacheLimit(limit);
    start(path, original.size());
    if (mode == 1) audit::error = ENOSYS;
    uint64_t next = 0;
    table.load(wal.committed, next);
    stop();
    require(mode == 0 ? audit::calls > 0 :
                audit::calls == (mode == 1 ? 1U : 0U), "cache gate changed advice");
    require(table.fullyCached() == (mode != 3), "cache fallback changed");
    require(table.rowCount() == 150 && next > 0, "recovery lost rows or txids");
    for (int key = 0; key != 150; ++key) {
      mvd::Row row;
      require(table.findBase(key, row) && row[1].text() == text,
              "recovery changed exact row contents");
    }
    require(bytes(path) == original && bytes(root / "mvd.wal") == wal_bytes &&
                bytes(root / "catalog.bin") == catalog_bytes,
            "read-side advice changed durable bytes");
  }
}
}

int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: test_table_load_pages EMPTY_TEMP_DIRECTORY");
    const mvd::fs::path root = argv[1];
    require(!mvd::fs::exists(root) || mvd::fs::is_empty(root), "fixture is not empty");
    mvd::fs::create_directories(root);
    checkReader(root);
    checkRecovery(root / "recovery");
    std::cout << "PASS table-load ranges, cache gating, errno, I/O errors and recovery\n";
  } catch (const std::exception &error) {
    audit::active = false;
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
#endif
