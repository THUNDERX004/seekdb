// Linux-only deterministic syscall audit; no page-residency or timing assertion.
// c++ -std=c++20 -O0 -DMVD_FORCE_SCALAR tests/test_durable_page_cache.cpp \
//   -Wl,--wrap=fsync -Wl,--wrap=sysconf -Wl,--wrap=posix_fadvise \
//   -o build/test_durable_page_cache
// Run: build/test_durable_page_cache EMPTY_TEMP_DIRECTORY
#ifndef __linux__
#include <iostream>
int main() { std::cout << "SKIP Linux durable-page advice audit\n"; }
#else
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace audit {
struct Advice { off_t begin; off_t length; ino_t inode; };
std::array<Advice, 512> calls{};
size_t count = 0;
size_t syncs = 0;
struct stat synced_file{};
bool synced = false;
bool invalid = false;
bool fail_sync = false;
bool real_advice = false;
int advice_error = 0;
long page_size = 4096;
constexpr int preserved_errno = EDOM;

void reset() {
  count = syncs = 0;
  synced = invalid = fail_sync = real_advice = false;
  advice_error = 0;
  page_size = 4096;
}
}  // namespace audit

extern "C" int __real_fsync(int descriptor);
extern "C" long __real_sysconf(int name);
extern "C" int __real_posix_fadvise(int descriptor, off_t begin, off_t length, int advice);
extern "C" int __wrap_fsync(int descriptor) {
  ++audit::syncs;
  audit::synced = false;
  if (audit::fail_sync) {
    audit::fail_sync = false;
    errno = EIO;
    return -1;
  }
  const int result = __real_fsync(descriptor);
  if (result == 0) {
    audit::synced = fstat(descriptor, &audit::synced_file) == 0;
    audit::invalid |= !audit::synced;
    // Make the errno entering the optional helper deterministic.
    errno = audit::preserved_errno;
  }
  return result;
}
extern "C" long __wrap_sysconf(int name) {
  if (name != _SC_PAGESIZE) { return __real_sysconf(name); }
  errno = ERANGE;
  return audit::page_size;
}
extern "C" int __wrap_posix_fadvise(int descriptor, off_t begin,
                                    off_t length, int advice) {
  struct stat file{};
  const bool valid_file = fstat(descriptor, &file) == 0;
  audit::invalid |= !valid_file || !audit::synced ||
      file.st_dev != audit::synced_file.st_dev ||
      file.st_ino != audit::synced_file.st_ino ||
      advice != POSIX_FADV_DONTNEED || begin < 0 || length <= 0 ||
      length > 65536 || audit::page_size <= 0;
  if (audit::page_size > 0) {
    audit::invalid |= begin % audit::page_size != 0 ||
        length % audit::page_size != 0 ||
        begin + length > audit::synced_file.st_size;
  }
  if (audit::count < audit::calls.size()) {
    audit::calls[audit::count++] = {begin, length, file.st_ino};
  } else {
    audit::invalid = true;
  }
  // Range tests simulate success; database tests also call the real advice.
  // Neither path requires that the filesystem actually evict any pages.
  const int result = audit::advice_error != 0 ? audit::advice_error
      : audit::real_advice ? __real_posix_fadvise(descriptor, begin, length, advice)
                          : 0;
  // Deliberately perturb errno even when the advice returns success.
  errno = EBUSY;
  return result;
}

#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
void require(bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

std::string bytes(const mvd::fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  require(static_cast<bool>(input), "cannot read durable fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void checkRanges(off_t begin, off_t end) {
  require(!audit::invalid, "advice was unaligned, unbounded or before fsync");
  for (size_t i = 0; i < audit::count; ++i) {
    require(audit::calls[i].begin == begin, "advice skipped or repeated a range");
    begin += audit::calls[i].length;
  }
  require(begin == end, "advice did not cover exactly the new complete pages");
}

void appendAndCheck(mvd::FileWriter &writer, const mvd::fs::path &path,
                    std::string &expected, size_t size, bool discard,
                    off_t begin, off_t end) {
  std::vector<uint8_t> payload(size, static_cast<uint8_t>('a' + expected.size() % 23));
  if (!payload.empty()) {
    expected.append(reinterpret_cast<const char *>(payload.data()), payload.size());
  }
  writer.append(payload);
  audit::reset();
  writer.sync(discard);
  require(errno == audit::preserved_errno, "advice leaked errno");
  require(audit::syncs == 1, "advice changed the durable sync count");
  checkRanges(begin, end);
  require(bytes(path) == expected, "advice changed durable bytes");
}

void checkWriter(const mvd::fs::path &root) {
  const auto path = root / "appends.dat";
  std::string expected;
  {
    mvd::FileWriter writer(path);
    appendAndCheck(writer, path, expected, 4095, true, 0, 0);
    appendAndCheck(writer, path, expected, 2, true, 0, 4096);
    appendAndCheck(writer, path, expected, 3 * 65536 + 17, true, 4096, 200704);
    // A repeated sync must not rescan already advised pages.
    appendAndCheck(writer, path, expected, 0, true, 0, 0);
  }
  {
    mvd::FileWriter writer(path);
    // A new writer must start at the existing tail, not the file's beginning.
    appendAndCheck(writer, path, expected, 4096, true, 200704, 204800);
    appendAndCheck(writer, path, expected, 8192, false, 0, 0);
    appendAndCheck(writer, path, expected, 4096, true, 212992, 217088);
  }
  for (long page_size : {-1L, 0L, 65536L, 131072L}) {
    const auto page_path = root / ("page-" + std::to_string(page_size));
    mvd::FileWriter writer(page_path);
    writer.append(std::vector<uint8_t>(2 * 65536 + 1, 42));
    audit::reset();
    audit::page_size = page_size;
    writer.sync(true);
    require(errno == audit::preserved_errno, "page-size lookup leaked errno");
    checkRanges(0, page_size == 65536 ? 2 * 65536 : 0);
  }
  const auto error_path = root / "advice-error.dat";
  {
    mvd::FileWriter writer(error_path);
    writer.append(std::vector<uint8_t>(3 * 65536, 73));
    audit::reset();
    audit::advice_error = ENOSYS;
    writer.sync(true);
    require(errno == audit::preserved_errno && !audit::invalid,
            "advice failure affected the caller");
    require(audit::syncs == 1 && audit::count == 1,
            "unsupported advice was retried or changed sync count");
  }
  require(bytes(error_path) == std::string(3 * 65536, 'I'),
          "advice failure changed durable bytes");
  {
    mvd::FileWriter writer(root / "sync-error.dat");
    writer.append(std::vector<uint8_t>(8192, 27));
    audit::reset();
    audit::fail_sync = true;
    bool failed = false;
    try { writer.sync(true); }
    catch (const mvd::DBError &error) { failed = error.code() == "IO_ERROR"; }
    require(failed && audit::syncs == 1 && audit::count == 0,
            "failed durable sync was suppressed or followed by advice");
  }
}

void checkDatabase(const mvd::fs::path &path, int advice_error) {
  require(setenv("MVD_CACHE_LIMIT_MB", "1", 1) == 0, "cannot set cache budget");
  const std::string big(60000, 'q');
  {
    mvd::Database database(path, false);
    audit::reset();
    // Keep every field within the SQL limit while exceeding the row-cache budget.
    std::string schema = "CREATE TABLE t(id BIGINT PRIMARY KEY";
    for (int column = 0; column < 10; ++column) {
      schema += ",s" + std::to_string(column) + " VARCHAR(65535)";
    }
    database.execute(schema + ")");
    require(audit::syncs == 1 && audit::count == 0, "catalog received advice");
    for (int key = 1; key <= 4; ++key) {
      audit::reset();
      audit::real_advice = true;
      audit::advice_error = advice_error;
      audit::page_size = __real_sysconf(_SC_PAGESIZE);
      const bool supported_pages = audit::page_size > 0 && audit::page_size <= 65536;
      const std::string text = key == 2 || key == 3 ? big : std::string(7000, 'r');
      std::string insert = "INSERT INTO t VALUES(" + std::to_string(key);
      for (int column = 0; column < 10; ++column) {
        insert += ",'" + text + "'";
      }
      database.execute(insert + ")");
      require(audit::syncs == 2, "data/WAL sync count changed");
      require(!audit::invalid, "database advised unsynced or invalid ranges");
      require(key == 4 || !supported_pages ? audit::count == 0 : audit::count > 0,
              "advice did not follow full-row-cache residency");
      struct stat table{};
      require(stat((path / "t1.dat").c_str(), &table) == 0, "cannot stat table");
      for (size_t i = 0; i < audit::count; ++i) {
        require(audit::calls[i].inode == table.st_ino, "WAL received advice");
      }
    }
    require(database.execute("SELECT id FROM t ORDER BY id")->rows ==
                std::vector<std::vector<std::string>>{{"1"}, {"2"}, {"3"}, {"4"}},
            "cache eviction lost rows");
  }
  const auto table_bytes = bytes(path / "t1.dat");
  const auto wal_bytes = bytes(path / "mvd.wal");
  const auto catalog_bytes = bytes(path / "catalog.bin");
  {
    mvd::Database recovered(path, false);
    require(recovered.execute("SELECT s0 FROM t WHERE id=2")->rows ==
                std::vector<std::vector<std::string>>{{big}},
            "advice changed exact durable contents after restart");
    require(recovered.execute("SELECT id FROM t ORDER BY id")->rows.size() == 4,
            "restart lost committed rows");
  }
  require(bytes(path / "t1.dat") == table_bytes &&
              bytes(path / "mvd.wal") == wal_bytes &&
              bytes(path / "catalog.bin") == catalog_bytes,
          "restart changed durable fixture bytes");
}
}  // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: test_durable_page_cache EMPTY_TEMP_DIRECTORY");
    const mvd::fs::path root = argv[1];
    require(!mvd::fs::exists(root) || mvd::fs::is_empty(root), "test directory is not empty");
    mvd::fs::create_directories(root);
    checkWriter(root);
    checkDatabase(root / "normal", 0);
    checkDatabase(root / "unsupported", ENOSYS);
    for (const char *name : {"t1.dat", "mvd.wal", "catalog.bin"}) {
      require(bytes(root / "normal" / name) == bytes(root / "unsupported" / name),
              "advice success/failure changed persistent format or bytes");
    }
    std::cout << "PASS durable bytes, bounded ranges, cache gating, errno, sync errors and restart\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
#endif
