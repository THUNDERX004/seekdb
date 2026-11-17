#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main
#include <random>
#ifndef _WIN32
#include <sys/mman.h>
#endif

uint32_t reference_crc(const uint8_t *bytes, size_t count) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < count; ++index) {
    crc ^= bytes[index];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

void check_crc(const uint8_t *bytes, size_t count) {
  if (mvd::crc32(bytes, count) != reference_crc(bytes, count))
    throw std::runtime_error("CRC differs from bitwise IEEE reference");
}

int main() {
  const std::string known = "123456789";
  if (mvd::crc32(reinterpret_cast<const uint8_t *>(known.data()), known.size()) != 0xcbf43926U)
    throw std::runtime_error("CRC known-answer mismatch");
  check_crc(nullptr, 0);
  std::mt19937 rng(20261118);
  std::vector<uint8_t> data(65536 + 64);
  for (auto &value : data) value = static_cast<uint8_t>(rng());
  for (size_t align = 0; align < 32; ++align) {
    for (size_t count = 0; count < 130; ++count) check_crc(data.data() + align, count);
    for (size_t count : {size_t{255}, size_t{256}, size_t{511}, size_t{512},
                         size_t{1023}, size_t{4096}, size_t{65536}})
      check_crc(data.data() + align, count);
  }
#ifndef _WIN32
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) throw std::runtime_error("page size unavailable");
  auto *memory = static_cast<uint8_t *>(mmap(nullptr, 2 * page,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (memory == MAP_FAILED) throw std::runtime_error("mmap failed");
  if (mprotect(memory + page, page, PROT_NONE) != 0)
    throw std::runtime_error("mprotect failed");
  for (long i = 0; i < page; ++i) memory[i] = static_cast<uint8_t>(rng());
  for (size_t count = 0; count < 257; ++count) check_crc(memory + page - count, count);
  if (munmap(memory, 2 * page) != 0) throw std::runtime_error("munmap failed");
#endif
  std::cout << "IEEE CRC reference, lengths, alignments and guard-page checks passed\n";
}
