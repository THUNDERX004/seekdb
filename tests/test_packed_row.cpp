// Standalone C++20 test; no database or build-system dependency.
#include "../src/packed_row.hpp"
#include "../src/compact_value.hpp"

#include <array>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace allocation_failure {
bool next_array = false;
}

// Fail precisely the staged packed allocation, without affecting strings or
// Layout's vector. This standalone executable is single-threaded.
void *operator new[](std::size_t bytes) {
  if (allocation_failure::next_array) {
    allocation_failure::next_array = false;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
  throw std::bad_alloc();
}
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
namespace pr = mvd::packed_row;
using Row = std::vector<mvd::Value>;

static_assert(sizeof(pr::PackedRow) == sizeof(std::unique_ptr<std::byte[]>));
static_assert(!std::is_copy_constructible_v<pr::PackedRow>);
static_assert(!std::is_copy_assignable_v<pr::PackedRow>);
static_assert(std::is_nothrow_move_constructible_v<pr::PackedRow>);
static_assert(std::is_nothrow_move_assignable_v<pr::PackedRow>);
static_assert(noexcept(std::declval<pr::PackedRow &>().swap(
    std::declval<pr::PackedRow &>())));
static_assert(sizeof(void *) != 8 || sizeof(pr::View) == 24);

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

constexpr std::array<pr::ColumnSpec, 8> kColumns{{
    {pr::Type::BigInt, 0, false},
    {pr::Type::VarChar, 64, false},
    {pr::Type::Vector, 3, false},
    {pr::Type::BigInt, 0, true},
    {pr::Type::VarChar, 64, false},
    {pr::Type::BigInt, 0, false},
    {pr::Type::Vector, 5, false},
    {pr::Type::Vector, 5, false},
}};
constexpr std::array<std::uint32_t, 3> kBits0{
    0x00000000, 0x80000000, 0x00000001};
constexpr std::array<std::uint32_t, 5> kBits1{
    0x007fffff, 0x00800000, 0x7f7fffff, 0xff7fffff, 0x3f800001};
constexpr std::array<std::uint32_t, 5> kBits2{
    0x7f800000, 0xff800000, 0x7fc12345, 0x7f800001, 0xffc54321};

std::vector<float> floats(std::span<const std::uint32_t> bits) {
  std::vector<float> result(bits.size());
  std::memcpy(result.data(), bits.data(), bits.size_bytes());
  return result;
}

Row source_row() {
  Row row(kColumns.size());
  row[0].setInteger(std::numeric_limits<std::int64_t>::min());
  row[1].setText(std::string("A\0\xE4\xB8\xAD", 5));
  row[2].setVector(floats(kBits0));
  row[3].setInteger(999);
  row[4].setText("");
  row[5].setInteger(std::numeric_limits<std::int64_t>::max());
  row[6].setVector(floats(kBits1));
  row[7].setVector(floats(kBits2));
  return row;
}

template <class Scalar>
void put(std::vector<std::byte> &bytes, std::size_t offset, Scalar value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// Independent native-endian oracle: offsets are hand-derived from the schema,
// never taken from Layout, Cell, or an encoded row. PK consumes no payload.
std::vector<std::byte> expected_bytes() {
  std::vector<std::byte> result(101, std::byte{0});
  put(result, 0, std::uint32_t{101});
  put(result, 8, std::numeric_limits<std::int64_t>::min());
  put(result, 16, std::uint32_t{88});
  std::memcpy(result.data() + 20, kBits0.data(), sizeof(kBits0));
  put(result, 32, std::uint32_t{97});
  put(result, 40, std::numeric_limits<std::int64_t>::max());
  std::memcpy(result.data() + 48, kBits1.data(), sizeof(kBits1));
  std::memcpy(result.data() + 68, kBits2.data(), sizeof(kBits2));
  put(result, 88, std::uint32_t{5});
  std::memcpy(result.data() + 92, "A\0\xE4\xB8\xAD", 5);
  put(result, 97, std::uint32_t{0});  // Deliberately unaligned length.
  return result;
}

void check_bytes(const pr::PackedRow &row, const pr::Layout &layout,
                 std::span<const std::byte> expected) {
  require(row.bytes() == expected.size(), "allocation size mismatch");
  require(std::memcmp(row.view(layout, 0).data(), expected.data(), expected.size()) == 0,
          "independent packed byte oracle mismatch");
}

void check_bits(std::span<const float> actual,
                std::span<const std::uint32_t> expected) {
  require(actual.size() == expected.size(), "vector size mismatch");
  require(reinterpret_cast<std::uintptr_t>(actual.data()) % alignof(float) == 0,
          "unaligned float payload");
  require(std::memcmp(actual.data(), expected.data(), expected.size_bytes()) == 0,
          "float32 bits changed");
}

void roundtrip_and_views() {
  const std::vector<pr::ColumnSpec> columns(kColumns.begin(), kColumns.end());
  const pr::Layout layout(columns);  // Integration passes vector through span.
  require(layout.supported() && layout.size() == 8, "valid layout rejected");
  pr::PackedRow packed;
  require(packed.bytes() == 0 && packed.view(layout, 1).size() == 0,
          "default row not empty");
  {
    Row row = source_row();
    require(packed.assign(layout, row), "valid assign rejected");
    row[0].setInteger(17);
    row[1].setText("changed");
    row[2].setVector({42.0f});
  }
  const auto expected = expected_bytes();
  check_bytes(packed, layout, expected);
  const auto view = packed.view(layout, -77);
  require(view.layout() == &layout && view.primary() == -77 && view.data(),
          "View metadata mismatch");
  const pr::View reconstructed(view.data(), view.layout(), view.primary());
  require(reconstructed.size() == 8 && reconstructed[3].integer() == -77,
          "reconstructed View or middle PK mismatch");
  require(view[0].integer() == std::numeric_limits<std::int64_t>::min() &&
              view[5].integer() == std::numeric_limits<std::int64_t>::max(),
          "integer range not preserved");
  require(view[1].text() == std::string_view("A\0\xE4\xB8\xAD", 5) &&
              view[4].text().empty(), "text bytes not preserved");
  check_bits(view[2].vector(), kBits0);
  check_bits(view[6].vector(), kBits1);
  check_bits(view[7].vector(), kBits2);
  for (std::size_t index = 0; index < view.size(); ++index) {
    const pr::Cell cell = view[index];
    if (kColumns[index].type != pr::Type::BigInt)
      require(cell.integer() == 0, "inactive integer is nonzero");
    if (kColumns[index].type != pr::Type::VarChar)
      require(cell.text().empty(), "inactive text is nonempty");
    if (kColumns[index].type != pr::Type::Vector)
      require(cell.vector().empty(), "inactive vector is nonempty");
  }
  const pr::Cell empty = view[view.size()];
  require(empty.integer() == 0 && empty.text().empty() && empty.vector().empty(),
          "empty Cell getters mismatch");
  require(pr::View{}.size() == 0 && pr::View(nullptr, &layout, 0).size() == 0 &&
              pr::View(view.data(), nullptr, 0).size() == 0,
          "empty View mismatch");
  // Source may borrow this very allocation: publish only after copying it all.
  require(packed.assign(layout, view), "assignment from own View rejected");
  check_bytes(packed, layout, expected);
}

void invalid_layouts() {
  const auto unsupported = [](std::initializer_list<pr::ColumnSpec> columns) {
    const pr::Layout layout(std::span<const pr::ColumnSpec>(columns.begin(), columns.size()));
    require(!layout.supported(), "invalid/overflowing layout accepted");
  };
  unsupported({});
  unsupported({{pr::Type::BigInt, 0, false}});
  unsupported({{pr::Type::BigInt, 0, true}, {pr::Type::BigInt, 0, true}});
  unsupported({{pr::Type::VarChar, 1, true}});
  unsupported({{pr::Type::Vector, 1, true}});
  unsupported({{pr::Type::BigInt, 1, true}});
  unsupported({{pr::Type::BigInt, 0, true}, {pr::Type::VarChar, 0, false}});
  unsupported({{pr::Type::BigInt, 0, true}, {pr::Type::Vector, 0, false}});
  unsupported({{pr::Type::BigInt, 0, true}, {static_cast<pr::Type>(255), 0, false}});
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  unsupported({{pr::Type::BigInt, 0, true}, {pr::Type::Vector, maximum, false}});
  unsupported({{pr::Type::BigInt, 0, true},
               {pr::Type::Vector, (maximum - 4) / 4, false},
               {pr::Type::BigInt, 0, false}});
}

struct ReportedText {
  std::size_t count;
  std::size_t size() const noexcept { return count; }
  const char *data() const noexcept { return nullptr; }
};
struct ReportedCell {
  std::size_t text_size = 0;
  std::int64_t integer() const noexcept { return 0; }
  ReportedText text() const noexcept { return {text_size}; }
  std::span<const float> vector() const noexcept { return {}; }
};

void failure_guarantees_and_utf8() {
  const pr::Layout layout(kColumns);
  const auto expected = expected_bytes();
  Row source = source_row();
  pr::PackedRow packed;
  require(packed.assign(layout, source), "initial assign failed");
  const std::byte *original = packed.view(layout, 0).data();
  const auto unchanged = [&] {
    require(packed.view(layout, 0).data() == original, "failed assign replaced allocation");
    check_bytes(packed, layout, expected);
  };
  require(!packed.assign(pr::Layout{}, source), "unsupported layout accepted");
  unchanged();
  Row short_row(7);
  require(!packed.assign(layout, short_row), "short row accepted");
  unchanged();
  source.emplace_back();
  require(!packed.assign(layout, source), "long row accepted");
  source.pop_back();
  unchanged();
  source[2].setVector({1.0f, 2.0f});
  require(!packed.assign(layout, source), "short vector accepted");
  unchanged();
  source[2].setVector({1.0f, 2.0f, 3.0f, 4.0f});
  require(!packed.assign(layout, source), "long vector accepted");
  unchanged();

  constexpr std::array<pr::ColumnSpec, 2> text_columns{{
      {pr::Type::BigInt, 0, true}, {pr::Type::VarChar, 1, false}}};
  const pr::Layout text_layout(text_columns);
  std::array<ReportedCell, 2> oversized{};
  oversized[1].text_size = std::numeric_limits<std::uint32_t>::max();
  require(!packed.assign(text_layout, oversized), "total-byte overflow accepted");
  unchanged();
  if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
    ++oversized[1].text_size;
    require(!packed.assign(text_layout, oversized), "string-byte overflow accepted");
    unchanged();
  }

  source = source_row();
  source[0].setInteger(17);
  allocation_failure::next_array = true;
  bool threw = false;
  try {
    (void)packed.assign(layout, source);
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  const bool consumed = !allocation_failure::next_array;
  allocation_failure::next_array = false;
  require(threw && consumed, "packed allocation failure was not propagated");
  unchanged();
  require(packed.assign(layout, source) && packed.view(layout, 0)[0].integer() == 17,
          "retry after allocation failure failed");

  Row utf8(2);
  utf8[1].setText("\xE4\xB8\xAD");  // One SQL codepoint, three bytes.
  require(packed.assign(text_layout, utf8), "valid UTF8 byte expansion rejected");
  require(packed.bytes() == 15 && packed.view(text_layout, 5)[1].text() == utf8[1].text(),
          "UTF8 bytes were not preserved");
}

void moves_reset_and_primary_only() {
  const pr::Layout layout(kColumns);
  const auto expected = expected_bytes();
  const Row source = source_row();
  pr::PackedRow first;
  require(first.assign(layout, source), "move fixture assign failed");
  const auto borrowed = first.view(layout, 19);
  pr::PackedRow second(std::move(first));
  require(first.bytes() == 0 && first.view(layout, 0).size() == 0,
          "move construction did not empty source");
  check_bytes(second, layout, expected);
  require(first.assign(layout, source), "occupied move fixture failed");
  first = std::move(second);
  require(second.bytes() == 0, "move assignment did not empty source");
  check_bytes(first, layout, expected);
  first.swap(second);
  require(first.bytes() == 0 && borrowed[3].integer() == 19,
          "swap or borrowed allocation across move failed");
  using std::swap;
  swap(first, second);
  check_bytes(first, layout, expected);
  first.reset();
  first.reset();
  require(first.bytes() == 0 && first.view(layout, 0).size() == 0,
          "reset did not empty row");
  require(first.assign(layout, source), "reuse after reset failed");
  check_bytes(first, layout, expected);

  constexpr std::array<pr::ColumnSpec, 1> primary{{{pr::Type::BigInt, 0, true}}};
  const pr::Layout primary_layout(primary);
  const Row primary_source(1);
  require(first.assign(primary_layout, primary_source), "primary-only assign failed");
  const auto view = first.view(primary_layout, std::numeric_limits<std::int64_t>::min());
  require(first.bytes() == 4 && view.size() == 1 &&
              view[0].integer() == std::numeric_limits<std::int64_t>::min(),
          "primary-only payload was not omitted");
}
}  // namespace

int main() {
  try {
    roundtrip_and_views();
    invalid_layouts();
    failure_guarantees_and_utf8();
    moves_reset_and_primary_only();
    std::cout << "packed row tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "packed row test failed: " << error.what() << '\n';
    return 1;
  }
}
