// Test data generators and assertions only. The implementation under test above
// is extracted verbatim from seekdb, including the old and proposed context code.
namespace {
uint64_t cases = 0, positive = 0, zero = 0, negative = 0;
uint64_t scalars = 0, surrogates = 0, one_byte = 0, two_byte = 0;
uint64_t boundary3 = 0, boundary4 = 0, truncations = 0, random_count = 0;
ObIAllocator allocator;
const std::array<ObCollationType, 3> utf8_collations{
  CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_UNICODE_CI
};

[[noreturn]] void fail(const char *what, const uchar *s, size_t size)
{
  std::ostringstream message;
  message << what << " case=" << cases << " bytes=";
  for (size_t i = 0; i < size; ++i) {
    message << std::hex << std::setfill('0') << std::setw(2) << unsigned(s[i]);
  }
  throw std::runtime_error(message.str());
}

// Independent encoding generates test inputs/expected codepoints, never a
// replacement for the validator or decoder being tested.
size_t encode(uint32_t cp, std::array<uchar, 8> &out)
{
  if (cp < 0x80) {
    out[0] = uchar(cp); return 1;
  } else if (cp < 0x800) {
    out[0] = uchar(0xc0 | (cp >> 6));
    out[1] = uchar(0x80 | (cp & 0x3f)); return 2;
  } else if (cp < 0x10000) {
    out[0] = uchar(0xe0 | (cp >> 12));
    out[1] = uchar(0x80 | ((cp >> 6) & 0x3f));
    out[2] = uchar(0x80 | (cp & 0x3f)); return 3;
  }
  out[0] = uchar(0xf0 | (cp >> 18));
  out[1] = uchar(0x80 | ((cp >> 12) & 0x3f));
  out[2] = uchar(0x80 | ((cp >> 6) & 0x3f));
  out[3] = uchar(0x80 | (cp & 0x3f)); return 4;
}

void check_case(const uchar *input, size_t size, int64_t expected_cp = -1, bool surrogate = false)
{
  ++cases;
  // Exact-sized allocation with an unaligned start: ASan can detect reads past
  // the logical end instead of silently accepting padding in a large buffer.
  std::unique_ptr<uchar[]> allocation(new uchar[size + 1]);
  uchar *s = allocation.get() + 1;
  if (size) std::memcpy(s, input, size);
  const uchar *end = s + size;
  ob_wc_t cp = 0;
  const int n = common::ob_charset_decode_unicode<CHARSET_UTF8MB4>(s, end, cp);
  int error = -1;
  const size_t validated = ob_well_formed_len_utf8mb4(nullptr,
      reinterpret_cast<const char *>(s), reinterpret_cast<const char *>(end), 1, &error);
  if (n > 0) {
    ++positive;
    ob_wc_t old_cp = 0, handler_cp = 0;
    const int old_n = common::ob_charset_decode_unicode<CHARSET_UTF8MB4>(s, s + n, old_cp);
    const int handler_n = ob_mb_wc_utf8mb4(nullptr, &handler_cp, s, s + n);
    if (error != 0 || validated != size_t(n)
        || ob_valid_mbcharlen_utf8mb4(nullptr, s, end) != n
        || old_n != n || old_cp != cp || handler_n != n || handler_cp != cp) {
      fail("positive decode does not imply identical old validation/decoding", s, size);
    }
  } else if (n == 0) {
    ++zero;
  } else {
    ++negative;
  }
  if (expected_cp >= 0) {
    if (cp != ob_wc_t(expected_cp) || (surrogate ? n != 0 : n <= 0)) {
      fail("unexpected scalar/surrogate decoder result", s, size);
    }
  }
  if (surrogate && (validated != 3 || error != 0)) {
    fail("surrogate acceptance of original validator changed", s, size);
  }

  // Prefix with a real valid first character to enable exactly the proposed
  // init transition, then exercise the actual old/new prepare method bodies.
  std::unique_ptr<char[]> document(new char[size + 1]);
  document[0] = 'A';
  if (size) std::memcpy(document.get() + 1, s, size);
  const auto coll = utf8_collations[cases % utf8_collations.size()];
  baseline::TokenizeContext old_context(coll, allocator, document.get(), int64_t(size + 1), false);
  candidate::TokenizeContext new_context(coll, allocator, document.get(), int64_t(size + 1), false);
  charset_lookups = 0;
  const int old_init = old_context.init();
  const uint64_t old_init_lookups = charset_lookups;
  charset_lookups = 0;
  const int new_init = new_context.init();
  if (old_init != OB_SUCCESS || new_init != old_init || old_init_lookups != 2
      || charset_lookups != old_init_lookups || !new_context.utf8_fast_path_) {
    fail("first character did not retain original path", s, size);
  }
  old_context.cursor_ = new_context.cursor_ = 1;
  old_context.next_char_len_ = new_context.next_char_len_ = 91;
  old_context.next_char_type_ = baseline::ObFTCharUtil::CharType::OTHER_CJK;
  new_context.next_char_type_ = candidate::ObFTCharUtil::CharType::OTHER_CJK;
  charset_lookups = 0;
  const int old_ret = old_context.prepare_next_char();
  const uint64_t old_lookups = charset_lookups;
  charset_lookups = 0;
  const int new_ret = new_context.prepare_next_char();
  if (new_ret != old_ret || new_context.next_char_len_ != old_context.next_char_len_
      || int(new_context.next_char_type_) != int(old_context.next_char_type_)) {
    fail("prepare return/length/type differs", s, size);
  }
  if ((n > 0 && charset_lookups != 0) || (n <= 0 && charset_lookups != old_lookups)) {
    fail("fast success or complete fallback lookup path differs", s, size);
  }
  if (surrogate && (old_ret != OB_SUCCESS || old_context.next_char_len_ != 3
      || int(old_context.next_char_type_) != 0)) {
    fail("surrogate must retain success/length3/USELESS", s, size);
  }
  if (size == 0 && (new_ret != OB_UNEXPECT_INTERNAL_ERROR
      || new_context.next_char_len_ != 0 || int(new_context.next_char_type_) != 0)) {
    fail("classification failure did not retain USELESS assignment", s, size);
  }
  if (size != 0 && error != 0 && (new_ret != OB_INVALID_ARGUMENT
      || new_context.next_char_len_ != 91 || int(new_context.next_char_type_) != 4)) {
    fail("validation failure modified cached length/type", s, size);
  }
}

void check_stream(const std::vector<uchar> &input)
{
  char empty = 0;
  const char *s = input.empty() ? &empty : reinterpret_cast<const char *>(input.data());
  for (const auto coll : utf8_collations) {
    baseline::TokenizeContext old_context(coll, allocator, s, int64_t(input.size()), true);
    candidate::TokenizeContext new_context(coll, allocator, s, int64_t(input.size()), true);
    old_context.next_char_type_ = baseline::ObFTCharUtil::CharType::OTHER_CJK;
    new_context.next_char_type_ = candidate::ObFTCharUtil::CharType::OTHER_CJK;
    int old_ret = old_context.init(), new_ret = new_context.init();
    for (size_t step = 0; step <= input.size() + 1; ++step) {
      if (old_ret != new_ret || old_context.cursor_ != new_context.cursor_
          || old_context.next_char_len_ != new_context.next_char_len_
          || int(old_context.next_char_type_) != int(new_context.next_char_type_)
          || old_context.handle_size_ != new_context.handle_size_) {
        fail("init/step stream state differs", input.data(), input.size());
      }
      if (old_ret != OB_SUCCESS) break;
      old_ret = old_context.step_next(); new_ret = new_context.step_next();
      if (step == input.size() + 1) fail("stream did not terminate", input.data(), input.size());
    }
    if (new_ret != OB_SUCCESS && new_context.cursor_ == 0 && new_context.utf8_fast_path_) {
      fail("failed init enabled UTF8 cache", input.data(), input.size());
    }
  }
}

void control_cases()
{
  const char text[] = "AB";
  for (const auto coll : {CS_TYPE_INVALID, CS_TYPE_BINARY, CS_TYPE_GBK_CHINESE_CI,
                          CS_TYPE_UTF16_GENERAL_CI, CS_TYPE_UTF16LE_GENERAL_CI}) {
    baseline::TokenizeContext old_context(coll, allocator, text, 2, false);
    candidate::TokenizeContext new_context(coll, allocator, text, 2, false);
    old_context.next_char_type_ = baseline::ObFTCharUtil::CharType::OTHER_CJK;
    new_context.next_char_type_ = candidate::ObFTCharUtil::CharType::OTHER_CJK;
    const int old_ret = old_context.init(), new_ret = new_context.init();
    if (old_ret != new_ret || new_context.utf8_fast_path_
        || old_context.next_char_len_ != new_context.next_char_len_
        || int(old_context.next_char_type_) != int(new_context.next_char_type_)) {
      fail("non-whitelisted collation or failed init changed", reinterpret_cast<const uchar *>(text), 2);
    }
  }
  for (const auto coll : utf8_collations) {
    candidate::TokenizeContext context(coll, allocator, text, 2, false);
    if (context.init() != OB_SUCCESS) throw std::runtime_error("init failed");
    charset_lookups = 0;
    if (context.init() != OB_SUCCESS || charset_lookups != 2) {
      throw std::runtime_error("repeated init did not use original first-character path");
    }
    candidate::TokenizeContext null_context(coll, allocator, nullptr, 3, false);
    if (null_context.init() != OB_INVALID_ARGUMENT || null_context.utf8_fast_path_) {
      throw std::runtime_error("null input enabled fast path");
    }
  }
  for (const auto cp : {0xff10UL, 0xff19UL, 0xff21UL, 0xff3aUL, 0xff41UL, 0xff5aUL}) {
    candidate::ObFTCharUtil::CharType type;
    candidate::ObFTCharUtil::classify_unicode<CHARSET_UTF8MB4>(cp, type);
    const int expected = cp < 0xff20 ? 1 : 2;
    if (int(type) != expected) throw std::runtime_error("fullwidth classification priority changed");
  }
  candidate::ObFTCharUtil::CharType type;
  candidate::ObFTCharUtil::classify_unicode<CHARSET_UTF16>(0xd800, type);
  if (int(type) != 5) throw std::runtime_error("UTF16 high surrogate classification changed");
  candidate::ObFTCharUtil::classify_unicode<CHARSET_UTF16LE>(0xdfff, type);
  if (int(type) != 6) throw std::runtime_error("UTF16LE low surrogate classification changed");
  candidate::ObFTCharUtil::classify_unicode<CHARSET_UTF8MB4>(0xd800, type);
  if (int(type) != 0) throw std::runtime_error("UTF8 acquired UTF16 surrogate classification");
}
} // namespace

int main(int argc, char **argv)
{
  try {
    const uint64_t requested_random = argc > 1 ? std::stoull(argv[1]) : 250000;
    control_cases();
    std::array<uchar, 8> bytes{};
    check_case(bytes.data(), 0);
    for (uint32_t cp = 0; cp <= 0x10ffff; ++cp) {
      if (cp >= 0xd800 && cp <= 0xdfff) continue;
      const size_t size = encode(cp, bytes);
      check_case(bytes.data(), size, cp); ++scalars;
    }
    for (uint32_t cp = 0xd800; cp <= 0xdfff; ++cp) {
      const size_t size = encode(cp, bytes);
      check_case(bytes.data(), size, cp, true); ++surrogates;
    }
    for (unsigned a = 0; a < 256; ++a) {
      bytes[0] = uchar(a); check_case(bytes.data(), 1); ++one_byte;
      for (unsigned b = 0; b < 256; ++b) {
        bytes[1] = uchar(b); check_case(bytes.data(), 2); ++two_byte;
      }
    }
    for (const unsigned a : {0xdf, 0xe0, 0xe1, 0xec, 0xed, 0xee, 0xef, 0xf0}) {
      bytes[0] = uchar(a);
      for (unsigned b = 0; b < 256; ++b) {
        bytes[1] = uchar(b);
        for (unsigned c = 0; c < 256; ++c) {
          bytes[2] = uchar(c); check_case(bytes.data(), 3); ++boundary3;
        }
      }
    }
    const std::array<unsigned, 10> tails{0x00, 0x7f, 0x80, 0x8f, 0x90, 0x9f, 0xa0, 0xbf, 0xc0, 0xff};
    for (const unsigned a : {0xf0, 0xf1, 0xf3, 0xf4, 0xf5, 0xf7, 0xf8, 0xff}) {
      bytes[0] = uchar(a);
      for (unsigned b = 0; b < 256; ++b) {
        bytes[1] = uchar(b);
        for (const unsigned c : tails) {
          bytes[2] = uchar(c);
          check_case(bytes.data(), 3); ++truncations;
          for (const unsigned d : tails) {
            bytes[3] = uchar(d); check_case(bytes.data(), 4); ++boundary4;
          }
        }
      }
    }
    std::mt19937_64 random(0x526f756e643130ULL);
    for (uint64_t index = 0; index < requested_random; ++index) {
      const size_t size = size_t(random() % 9);
      for (size_t j = 0; j < size; ++j) bytes[j] = uchar(random());
      check_case(bytes.data(), size); ++random_count;
      // A canonical first character followed by arbitrary (possibly invalid)
      // bytes proves that the fast decoder does not validate the whole suffix.
      uint32_t cp = uint32_t(random() % 0x110000);
      if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
      const size_t prefix = encode(cp, bytes);
      const size_t suffix = size_t(random() % 5);
      for (size_t j = 0; j < suffix; ++j) bytes[prefix + j] = uchar(random());
      check_case(bytes.data(), prefix + suffix, cp); ++random_count;
      if (index < 2000) check_stream(std::vector<uchar>(bytes.begin(), bytes.begin() + prefix + suffix));
    }
    const std::vector<std::vector<uchar>> streams{
        {}, {'A', 'B', ' ', '9', 0, 'Z'},
        {0xed, 0xa0, 0x80, 'A'}, {'A', 0xed, 0xbf, 0xbf, 'B'},
        {'A', 0xe4, 0xb8, 0xad, 'B', 0xf4, 0x8f, 0xbf, 0xbf},
        {'A', 0xe0, 0x80, 0x80}, {'A', 0xf4, 0x90, 0x80, 0x80},
        {'A', 0xf0, 0x90, 0x80}, {'A', 0xff}};
    for (const auto &stream : streams) {
      check_stream(stream);
    }
    std::cout << "PASS cases=" << cases << " positive=" << positive << " zero=" << zero
              << " negative=" << negative << " scalar=" << scalars << " surrogate=" << surrogates
              << " all1=" << one_byte << " all2=" << two_byte << " boundary3=" << boundary3
              << " boundary4=" << boundary4 << " truncation3=" << truncations
              << " random=" << random_count << "\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << "\n";
    return 1;
  }
}
