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

void scalar_column_bindings() {
  const pr::Layout layout(kColumns);
  const auto first = layout.scalarColumn(0);
  const auto text = layout.scalarColumn(1);
  const auto primary = layout.scalarColumn(3);
  const auto empty = layout.scalarColumn(4);
  const auto middle = layout.scalarColumn(5);
  require(first.offset == 8 && !first.primary &&
              text.offset == 16 && !text.primary &&
              primary.offset == 0 && primary.primary &&
              empty.offset == 32 && !empty.primary &&
              middle.offset == 40 && !middle.primary,
          "scalar bindings differ from independent fixed-slot offsets");
  const auto check = [&](const std::byte *data, std::int64_t key,
                         std::int64_t first_value, std::string_view first_text,
                         std::string_view second_text, std::int64_t middle_value) {
    require(first.integer(data, key) == first_value &&
                middle.integer(data, key) == middle_value,
            "bound integer read changed");
    require(primary.integer(data, key) == key,
            "bound primary read used omitted payload instead of key");
    require(text.text(data) == first_text && empty.text(data) == second_text,
            "bound text bytes or length changed");
  };
  const auto expected = expected_bytes();
  const std::string_view nul_utf8("A\0\xE4\xB8\xAD", 5);
  // Check the accessors against hand-written bytes without invoking assign.
  check(expected.data(), -77, std::numeric_limits<std::int64_t>::min(),
        nul_utf8, {}, std::numeric_limits<std::int64_t>::max());
  pr::PackedRow packed;
  Row row = source_row();
  require(packed.assign(layout, row), "scalar binding fixture rejected");
  check(packed.view(layout, 0).data(), -77,
        std::numeric_limits<std::int64_t>::min(), nul_utf8, {},
        std::numeric_limits<std::int64_t>::max());

  row[0].setInteger(-123);
  row[1].setText("");
  row[3].setInteger(9999);
  row[4].setText(std::string("\xC3\xA9\0Z", 4));
  row[5].setInteger(456);
  // Same inline vectors/slots, different text tails: T1=88, T4=92.
  std::vector<std::byte> second_expected(100, std::byte{0});
  put(second_expected, 0, std::uint32_t{100});
  put(second_expected, 8, std::int64_t{-123});
  put(second_expected, 16, std::uint32_t{88});
  std::memcpy(second_expected.data() + 20, kBits0.data(), sizeof(kBits0));
  put(second_expected, 32, std::uint32_t{92});
  put(second_expected, 40, std::int64_t{456});
  std::memcpy(second_expected.data() + 48, kBits1.data(), sizeof(kBits1));
  std::memcpy(second_expected.data() + 68, kBits2.data(), sizeof(kBits2));
  put(second_expected, 88, std::uint32_t{0});
  put(second_expected, 92, std::uint32_t{4});
  std::memcpy(second_expected.data() + 96, "\xC3\xA9\0Z", 4);
  const std::string_view second_text("\xC3\xA9\0Z", 4);
  check(second_expected.data(), 81, -123, {}, second_text, 456);
  pr::PackedRow second;
  require(second.assign(layout, row), "second scalar binding fixture rejected");
  check_bytes(second, layout, second_expected);
  check(second.view(layout, 0).data(), 81, -123, {}, second_text, 456);
  check(packed.view(layout, 0).data(), 82,
        std::numeric_limits<std::int64_t>::min(), nul_utf8, {},
        std::numeric_limits<std::int64_t>::max());
  const auto borrowed = second.view(layout, -88);
  require(second.assign(layout, borrowed), "bound row self-view assign rejected");
  check_bytes(second, layout, second_expected);
  check(second.view(layout, 0).data(), -88, -123, {}, second_text, 456);
}

void vector_slot_order_and_alignment() {
  // Vectors occupy the first, middle, and final schema positions. Text appears
  // on both sides of a vector; only text payloads go into the tail.
  constexpr std::array<pr::ColumnSpec, 7> columns{{
      {pr::Type::Vector, 3, false}, {pr::Type::VarChar, 4, false},
      {pr::Type::BigInt, 0, true}, {pr::Type::Vector, 5, false},
      {pr::Type::VarChar, 2, false}, {pr::Type::BigInt, 0, false},
      {pr::Type::Vector, 5, false},
  }};
  const pr::Layout layout(columns);
  require(layout.supported(), "vector-slot fixture layout rejected");
  Row row(columns.size());
  row[0].setVector(floats(kBits0));
  row[2].setInteger(999);
  row[3].setVector(floats(kBits1));
  row[4].setText("yz");
  row[5].setInteger(-123456789);
  row[6].setVector(floats(kBits2));
  pr::PackedRow packed;
  for (std::size_t length = 0; length < 4; ++length) {
    row[1].setText(std::string("\0\xC3\xA9", length));
    // Hand-derived fixed slots: V0=4, T1=16, V3=20, T4=40, I5=48,
    // V6=56. Fixed end=76; text headers/bytes end at 86+length.
    std::vector<std::byte> expected(86 + length, std::byte{0});
    put(expected, 0, static_cast<std::uint32_t>(expected.size()));
    std::memcpy(expected.data() + 4, kBits0.data(), sizeof(kBits0));
    put(expected, 16, std::uint32_t{76});
    std::memcpy(expected.data() + 20, kBits1.data(), sizeof(kBits1));
    put(expected, 40, static_cast<std::uint32_t>(80 + length));
    put(expected, 48, std::int64_t{-123456789});
    std::memcpy(expected.data() + 56, kBits2.data(), sizeof(kBits2));
    put(expected, 76, static_cast<std::uint32_t>(length));
    if (length != 0) std::memcpy(expected.data() + 80, "\0\xC3\xA9", length);
    put(expected, 80 + length, std::uint32_t{2});
    std::memcpy(expected.data() + 84 + length, "yz", 2);
    require(packed.assign(layout, row), "vector-slot assign rejected");
    check_bytes(packed, layout, expected);
    const auto view = packed.view(layout, -77);
    require(view[2].integer() == -77 && view[5].integer() == -123456789,
            "mixed fixed integer/PK mismatch");
    require(view[1].text() == row[1].text() && view[4].text() == "yz",
            "text around schema vectors changed");
    check_bits(view[0].vector(), kBits0);
    check_bits(view[3].vector(), kBits1);
    check_bits(view[6].vector(), kBits2);
    require(reinterpret_cast<const std::byte *>(view[0].vector().data()) == view.data() + 4 &&
                reinterpret_cast<const std::byte *>(view[3].vector().data()) == view.data() + 20 &&
                reinterpret_cast<const std::byte *>(view[6].vector().data()) == view.data() + 56,
            "vector getters did not use independent inline offsets");
  }
}

void inline_vector_before_text_tail() {
  constexpr std::array<pr::ColumnSpec, 4> columns{{
      {pr::Type::BigInt, 0, true}, {pr::Type::VarChar, 16, false},
      {pr::Type::VarChar, 64, false}, {pr::Type::Vector, 128, false},
  }};
  const pr::Layout layout(columns);
  Row row(columns.size());
  row[1].setText("c9");
  row[2].setText("row42");
  std::array<std::uint32_t, 128> bits{};
  for (std::size_t axis = 0; axis < bits.size(); ++axis)
    bits[axis] = kBits2[axis % kBits2.size()];
  row[3].setVector(floats(bits));
  std::vector<std::byte> expected(539, std::byte{0});
  put(expected, 0, std::uint32_t{539});
  put(expected, 4, std::uint32_t{524});
  put(expected, 8, std::uint32_t{530});
  std::memcpy(expected.data() + 12, bits.data(), sizeof(bits));
  put(expected, 524, std::uint32_t{2});
  std::memcpy(expected.data() + 528, "c9", 2);
  put(expected, 530, std::uint32_t{5});
  std::memcpy(expected.data() + 534, "row42", 5);
  pr::PackedRow packed;
  require(packed.assign(layout, row), "inline-vector fixture assign rejected");
  check_bytes(packed, layout, expected);
  const auto view = packed.view(layout, 42);
  require(view[0].integer() == 42 && view[1].text() == "c9" && view[2].text() == "row42",
          "inline-vector roundtrip changed");
  require(reinterpret_cast<const std::byte *>(view[3].vector().data()) == view.data() + 12 &&
              reinterpret_cast<const std::byte *>(view[1].text().data()) == view.data() + 528 &&
              reinterpret_cast<const std::byte *>(view[2].text().data()) == view.data() + 534,
          "inline vector or text tail offsets changed");
  check_bits(view[3].vector(), bits);
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
  const auto integer_binding = layout.scalarColumn(0);
  const auto text_binding = layout.scalarColumn(1);
  const auto primary_binding = layout.scalarColumn(3);
  const auto empty_binding = layout.scalarColumn(4);
  const auto middle_binding = layout.scalarColumn(5);
  const std::byte *original = packed.view(layout, 0).data();
  const auto unchanged = [&] {
    require(packed.view(layout, 0).data() == original, "failed assign replaced allocation");
    check_bytes(packed, layout, expected);
    require(integer_binding.integer(original, -77) ==
                std::numeric_limits<std::int64_t>::min() &&
                middle_binding.integer(original, -77) ==
                    std::numeric_limits<std::int64_t>::max() &&
                primary_binding.integer(original, -77) == -77 &&
                text_binding.text(original) == std::string_view("A\0\xE4\xB8\xAD", 5) &&
                empty_binding.text(original).empty(),
            "failed assign changed bound scalar reads");
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
  const auto primary_binding = primary_layout.scalarColumn(0);
  require(primary_binding.offset == 0 && primary_binding.primary &&
              primary_binding.integer(view.data(), view.primary()) ==
                  std::numeric_limits<std::int64_t>::min() &&
              primary_binding.integer(view.data(), 42) == 42,
          "schema-first primary binding read the four-byte header as payload");
}
}  // namespace

int main() {
  try {
    roundtrip_and_views();
    scalar_column_bindings();
    vector_slot_order_and_alignment();
    inline_vector_before_text_tail();
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
