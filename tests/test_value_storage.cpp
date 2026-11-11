#define main minivectordb_application_main
#include "../src/minivectordb.cpp"
#undef main

namespace {
void require(bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

void checkEncoding() {
  struct PreviousCell {
    int64_t integer = 0;
    std::string text;
    std::vector<float> vector;
  };
  static_assert(sizeof(mvd::Value) < sizeof(PreviousCell));
  mvd::TableDef definition;
  definition.columns = {{"id", mvd::ColumnType::BigInt, 0},
                        {"n", mvd::ColumnType::BigInt, 0},
                        {"empty", mvd::ColumnType::VarChar, 30},
                        {"nul", mvd::ColumnType::VarChar, 30},
                        {"long", mvd::ColumnType::VarChar, 30},
                        {"v", mvd::ColumnType::Vector, 4}};
  mvd::Row row(6);
  row[0].setInteger(std::numeric_limits<int64_t>::min());
  row[1].setInteger(std::numeric_limits<int64_t>::max());
  row[2].setText("");
  row[3].setText(std::string("nul\0byte", 8));
  row[4].setText("a sufficiently long string");
  const std::array<uint32_t, 4> bits{0x80000000, 1, 0x7f7fffff, 0x80000001};
  std::vector<float> components;
  for (uint32_t value : bits) { components.push_back(std::bit_cast<float>(value)); }
  row[5].setVector(std::move(components));

  // Independent fixed little-endian fixture, generated with Python struct.
  // No database serialization helper participates in the expected byte array.
  constexpr std::string_view hex =
      "0600000000000000000000800000000000000000ffffffffffffff7f000000000000000000000000"
      "0000000000000000000000000000000000000000080000006e756c00627974650000000000000000"
      "000000001a000000612073756666696369656e746c79206c6f6e6720737472696e67000000000000"
      "00000000000000000000040000000000008001000000ffff7f7f01000080";
  std::vector<uint8_t> golden;
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < hex.size(); i += 2) {
    golden.push_back(static_cast<uint8_t>(digit(hex[i]) * 16 + digit(hex[i + 1])));
  }
  require(golden.size() == 150, "invalid golden row fixture");
  require(mvd::serializeRow(row) == golden, "compact cells changed durable row bytes");
  const mvd::Row decoded = mvd::deserializeRow(golden, definition);
  require(decoded[0].integer() == std::numeric_limits<int64_t>::min() &&
              decoded[1].integer() == std::numeric_limits<int64_t>::max(),
          "integer limits changed after decoding");
  require(decoded[2].text().empty() && decoded[3].text() == std::string("nul\0byte", 8) &&
              decoded[4].text() == "a sufficiently long string",
          "empty, NUL or allocated text changed after decoding");
  require(decoded[5].vector().size() == bits.size(), "decoded vector size changed");
  for (size_t i = 0; i < bits.size(); ++i) {
    require(std::bit_cast<uint32_t>(decoded[5].vector()[i]) == bits[i],
            "signed zero, subnormal or maximum float bits changed");
  }
  require(mvd::serializeRow(decoded) == golden, "decoded row failed the golden roundtrip");

  mvd::Row copy = decoded;
  copy[4].setText("replaced");
  copy[5].setVector({1, 2, 3, 4});
  copy[0].setText("new alternative");
  require(copy[0].integer() == 0 && copy[0].vector().empty(),
          "inactive cell accessors did not retain empty defaults");
  require(mvd::serializeRow(decoded) == golden, "copied cells shared mutable payloads");
  std::swap(copy[0], copy[5]);
  require(copy[0].vector() == std::vector<float>({1, 2, 3, 4}) &&
              copy[5].text() == "new alternative", "alternative swap lost its payload");
  std::cout << "PASS compact cells: " << sizeof(PreviousCell) << " -> "
            << sizeof(mvd::Value) << " bytes, golden encoding, copy and swap\n";
}
}  // namespace

int main() {
  try { checkEncoding(); }
  catch (const std::exception &error) {
    std::cerr << "FAIL compact cell regression: " << error.what() << '\n';
    return 1;
  }
}
