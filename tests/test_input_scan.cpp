// Standalone correctness checks; no DB, timer, or framework.
// Build later: g++ -std=c++20 -O2 test_input_scan.cpp -o test_input_scan
// Also build with -DMVD_FORCE_SCALAR to exercise portable-only dispatch.
#include "../src/input_scan.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
namespace scan = mvd::input_scan;
using Sql = std::size_t (*)(const char *, std::size_t, char) noexcept;
using Json = std::size_t (*)(const char *, std::size_t) noexcept;
struct Entry { const char *name; Sql sql; Json json; Json numeric_array; };
std::vector<Entry> entries{
    {"scalar", &scan::sql_scalar, &scan::json_scalar,
     &scan::numeric_array_prefix_scalar},
    {"dispatch", &scan::sql_special, &scan::json_special,
     &scan::numeric_array_prefix}};
std::size_t checked = 0;

// Independent byte-only oracles; deliberately no SWAR/SIMD masks.
std::size_t sql_oracle(const char *s, std::size_t n, char quote) {
  for (std::size_t i = 0; i < n; ++i) {
    if (s[i] == quote || s[i] == '\\') return i;
  }
  return n;
}
std::size_t json_oracle(const char *s, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned byte = static_cast<unsigned char>(s[i]);
    if (byte < 32 || byte > 127 || byte == '"' || byte == '\\') return i;
  }
  return n;
}
std::size_t numeric_array_oracle(const char *s, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    // Explicit byte cases keep this independent of the implementation's
    // range comparisons and SIMD masks. This is an alphabet, not a grammar.
    switch (static_cast<unsigned char>(s[i])) {
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
      case 'e': case 'E': case '+': case '.': case ',':
      case ' ': case '\t': case '\n': case '\v': case '\f': case '\r':
        break;
      case '-':
        if (i + 1 < n && s[i + 1] == '-') return i;
        break;
      default: return i;
    }
  }
  return n;
}
void check(const char *s, std::size_t n) {
  const auto json = json_oracle(s, n);
  const auto numeric_array = numeric_array_oracle(s, n);
  for (const auto &entry : entries) {
    const auto mismatch = [&](const char *kind) {
      throw std::runtime_error(std::string(entry.name) + " " + kind +
          " result differs: n=" +
          std::to_string(n) + " alignment=" +
          std::to_string(reinterpret_cast<std::uintptr_t>(s) % 64));
    };
    if (entry.json(s, n) != json) mismatch("JSON");
    if (entry.numeric_array(s, n) != numeric_array) mismatch("numeric array");
    for (char quote : {'\'', '"'}) {
      if (entry.sql(s, n, quote) != sql_oracle(s, n, quote)) mismatch("SQL");
    }
  }
  ++checked;
}

void check_boundaries() {
  std::vector<char> buffer(4097 + 128, 'A');
  for (std::size_t alignment = 0; alignment < 64; ++alignment) {
    char *s = buffer.data() + alignment;
    // All short lengths and byte alignments, including direct AVX2 calls with
    // n < 32 and n == 0. Public dispatch would use SWAR for those lengths.
    for (std::size_t n = 0; n <= 97; ++n) check(s, n);
    check(s, 4097);
    for (std::size_t position : {31U, 32U, 63U, 64U}) {
      // Every byte at each SIMD handoff; the existing public-parser suite
      // covers the broader UTF-8/escape/invalid-JSON acceptance matrix.
      for (unsigned byte = 0; byte < 256; ++byte) {
        s[position] = static_cast<char>(byte);
        check(s, 65);
      }
      s[position] = 'A';
    }
    for (char quote : {'\'', '"'}) {
      s[31] = quote; s[32] = quote; check(s, 65);  // doubled quote across 32
      s[31] = '\\'; s[32] = quote; check(s, 65); // escape across 32
      s[31] = 'A'; s[32] = 'A';
    }
    s[31] = static_cast<char>(0xc3); s[32] = static_cast<char>(0xa9);
    check(s, 65);  // valid UTF-8 must stop at its first high byte, not skip it
    s[31] = static_cast<char>(0xc0); s[32] = static_cast<char>(0xaf);
    check(s, 65);  // malformed UTF-8 must reach the decoder too
    s[31] = 'A'; s[32] = 'A';
  }
}

void check_numeric_array_boundaries() {
  std::vector<char> buffer(4097 + 128, '0');
  for (std::size_t alignment = 0; alignment < 64; ++alignment) {
    char *s = buffer.data() + alignment;
    std::fill_n(s, 4097, '0');
    for (std::size_t n = 0; n <= 97; ++n) check(s, n);
    for (std::size_t n : {127U, 128U, 129U, 255U, 256U, 257U,
                          4095U, 4096U, 4097U}) check(s, n);
    // Every byte in every SIMD lane, on both sides of a full block and tail.
    // Unlike the SQL/JSON fixture, the prefix is allowed numeric-array data.
    for (std::size_t position = 0; position <= 65; ++position) {
      for (unsigned byte = 0; byte < 256; ++byte) {
        s[position] = static_cast<char>(byte);
        check(s, position + 1);
        check(s, 129);
      }
      s[position] = '0';
    }
    for (std::size_t position = 0; position < 129; ++position) {
      s[position] = '-';
      s[position + 1] = '-';
      check(s, position + 1);  // only the first '-' is inside the span
      check(s, position + 2);  // '--' ends at the exact readable boundary
      check(s, 257);
      s[position + 2] = ']';
      check(s, 257);  // a comment must win over a later special byte
      s[position + 2] = '0';
      if (position != 0) {
        s[position - 1] = '#';
        check(s, 257);  // an earlier special must win over the comment
        s[position - 1] = '0';
      }
      s[position] = '0';
      s[position + 1] = '0';
    }
    for (std::size_t position : {31U, 32U, 63U, 64U, 95U, 96U}) {
      s[position - 1] = '-';
      for (unsigned byte = 0; byte < 256; ++byte) {
        s[position] = static_cast<char>(byte);
        check(s, position + 1);
        check(s, 129);
      }
      s[position - 1] = '0';
      s[position] = '0';
    }
  }
  for (const std::string text : {
           "0123456789eE+-,. \t\n\v\f\r", "-", "--", "---", "- -", "-\n-",
           "1e+,2]", "1,--\n2]", "1,--comment\n2]", "1,/*x*/2]",
           "1,#x\n2]", "1,[2]]", "1,'2']", "1,\"2\"]", "1,\\2]"}) {
    for (std::size_t n = 0; n <= text.size(); ++n) check(text.data(), n);
  }
}

#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
class GuardedPages {
 public:
  GuardedPages() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const std::size_t page = info.dwPageSize;
#else
    const long page_value = sysconf(_SC_PAGESIZE);
    if (page_value <= 0) throw std::runtime_error("cannot determine page size");
    const auto page = static_cast<std::size_t>(page_value);
#endif
    const std::size_t readable = (4097 + 63 + page - 1) / page * page;
    size_ = readable + 2 * page;
#if defined(_WIN32)
    mapping_ = VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (mapping_ == nullptr) throw std::runtime_error("VirtualAlloc failed");
    begin = static_cast<char *>(mapping_) + page;
    DWORD previous = 0;
    if (!VirtualProtect(begin, readable, PAGE_READWRITE, &previous)) {
      VirtualFree(mapping_, 0, MEM_RELEASE);
      throw std::runtime_error("VirtualProtect failed");
    }
#else
    mapping_ = mmap(nullptr, size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping_ == MAP_FAILED) throw std::runtime_error("mmap failed");
    begin = static_cast<char *>(mapping_) + page;
    if (mprotect(begin, readable, PROT_READ | PROT_WRITE) != 0) {
      munmap(mapping_, size_);
      throw std::runtime_error("mprotect failed");
    }
#endif
    end = begin + readable;
  }
  ~GuardedPages() {
#if defined(_WIN32)
    VirtualFree(mapping_, 0, MEM_RELEASE);
#else
    munmap(mapping_, size_);
#endif
  }
  GuardedPages(const GuardedPages &) = delete;
  GuardedPages &operator=(const GuardedPages &) = delete;
  char *begin = nullptr;
  char *end = nullptr;

 private:
  void *mapping_ = nullptr;
  std::size_t size_ = 0;
};

bool check_guard_pages() {
  GuardedPages pages;
  for (std::size_t n : {0U, 1U, 7U, 8U, 9U, 15U, 16U, 17U, 23U, 24U, 25U,
                         30U, 31U, 32U, 33U, 39U, 40U, 47U, 48U, 55U, 56U,
                         62U, 63U, 64U, 65U, 95U, 96U, 97U, 127U, 128U, 129U,
                         255U, 256U, 257U, 4095U, 4096U, 4097U}) {
    for (std::size_t offset = 0; offset < 64; ++offset) {
      // offset==0 has an exact inaccessible right/left boundary. Other
      // offsets exercise every alignment near either guard.
      for (char *s : {pages.end - n - offset, pages.begin + offset}) {
        std::fill_n(s, n, 'A');
        check(s, n);
        if (n != 0) {
          for (unsigned byte : {0U, 1U, 31U, 32U, 34U, 39U, 92U, 127U, 128U, 255U}) {
            s[n - 1] = static_cast<char>(byte);
            check(s, n);
          }
          s[n - 1] = 'A';
        }
        // Force the numeric scanner to reach either guard, including a lone
        // final '-', a '--' at the end, and pairs across vector/tail handoffs.
        std::fill_n(s, n, '0');
        check(s, n);
        if (n != 0) {
          s[n - 1] = '-';
          check(s, n);
          for (unsigned byte : {0U, 8U, 9U, 10U, 11U, 12U, 13U, 14U,
                                32U, 35U, 43U, 44U, 45U, 46U, 47U, 48U,
                                57U, 58U, 69U, 91U, 93U, 101U, 127U, 128U, 255U}) {
            s[n - 1] = static_cast<char>(byte);
            check(s, n);
          }
          s[n - 1] = '0';
        }
        if (n > 1) {
          for (std::size_t position : {std::size_t{0}, std::size_t{7},
                  std::size_t{15}, std::size_t{30}, std::size_t{31},
                  std::size_t{32}, std::size_t{62}, std::size_t{63}, n - 2}) {
            if (position + 1 >= n) continue;
            s[position] = '-';
            s[position + 1] = '-';
            check(s, n);
            s[position] = '0';
            s[position + 1] = '0';
          }
        }
      }
    }
  }
  return true;
}
#else
bool check_guard_pages() { return false; }
#endif
}  // namespace

int main(int argc, char **argv) {
  try {
    bool have_avx2 = false;
#if !defined(MVD_FORCE_SCALAR) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
    have_avx2 = __builtin_cpu_supports("avx2");
    if (have_avx2) entries.push_back({"direct AVX2", &scan::sql_avx2,
                                    &scan::json_avx2, &scan::numeric_array_prefix_avx2});
#endif
    bool require_guards = false;
    for (int i = 1; i < argc; ++i) {
      const std::string argument(argv[i]);
      if (argument == "--require-avx2") {
        if (!have_avx2) throw std::runtime_error("AVX2 required but unavailable");
      } else if (argument == "--require-guard-pages") {
        require_guards = true;
      } else {
        throw std::runtime_error("unknown option: " + argument);
      }
    }
    check_boundaries();
    check_numeric_array_boundaries();
    const bool guards = check_guard_pages();
    if (require_guards && !guards) throw std::runtime_error("guard pages unavailable");
    std::cout << "PASS spans=" << checked << " direct_avx2=" << have_avx2
              << " guard_pages=" << guards << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
