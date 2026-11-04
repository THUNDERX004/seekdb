// Direct production-checker regression. Link with the normal seekdb storage,
// charset and runtime test dependencies; this is not an algorithm-only leaf.
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "lib/charset/ob_charset.h"
#include "lib/charset/ob_ctype.h"
#include "storage/fts/ob_fts_stop_word.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::storage;

namespace {
void require(const bool condition, const char *message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ObObjMeta meta(const ObCollationType coll)
{
  ObObjMeta result;
  result.set_varchar();
  result.set_collation_type(coll);
  return result;
}

// The original path's real charset conversion, real generic hash and real
// collation equality remain the oracle. No local case-fold/hash substitute.
class OriginalSet {
public:
  OriginalSet()
  {
    require(set_.create(37, "StopAsciiTest", "StopAsciiTest") == OB_SUCCESS, "reference create");
    for (const auto &text : ob_stop_word_list) {
      const ObFTWord word(STRLEN(text), text, meta(CS_TYPE_UTF8MB4_GENERAL_CI));
      require(set_.set_refactored(word) == OB_SUCCESS, "reference insert");
    }
  }
  ~OriginalSet() { set_.destroy(); }
  int check(const ObFTWord &word, bool &result)
  {
    if (word.empty()) {
      return OB_INVALID_ARGUMENT;
    }
    ObArenaAllocator allocator(lib::ObMemAttr("StopAsciiTest"));
    ObString converted;
    int ret = ObCharset::charset_convert(allocator, word.get_word().get_string(),
        word.get_collation_type(), CS_TYPE_UTF8MB4_GENERAL_CI, converted);
    if (ret == OB_SUCCESS) {
      const ObFTWord target(converted.length(), converted.ptr(), meta(CS_TYPE_UTF8MB4_GENERAL_CI));
      ret = set_.exist_refactored(target);
      if (ret == OB_HASH_EXIST || ret == OB_HASH_NOT_EXIST) {
        result = ret == OB_HASH_EXIST;
        ret = OB_SUCCESS;
      } else if (ret == OB_SUCCESS) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    return ret;
  }
private:
  hash::ObHashSet<ObFTWord, hash::NoPthreadDefendMode> set_;
};

uint64_t checks = 0;
void compare(ObStopWordChecker &checker, OriginalSet &original,
             const std::string &text, const ObCollationType coll)
{
  const ObFTWord word(text.size(), text.data(), meta(coll));
  for (const bool sentinel : {false, true}) {
    bool expected = sentinel;
    bool actual = sentinel;
    const int old_ret = original.check(word, expected);
    const int new_ret = checker.check_stopword(word, actual);
    require(new_ret == old_ret && actual == expected, "checker/reference return or output differs");
    ++checks;
  }
}
} // namespace

int main()
{
  try {
    require(ObCharset::init_charset() == OB_SUCCESS, "charset init");
    const ObCharsetInfo *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
    require(cs != nullptr && cs->coll != nullptr, "general-ci descriptor");
    // Check all 52x52 ASCII letter pairs through the actual PAD SPACE comparator.
    for (unsigned i = 0; i < 52; ++i) {
      for (unsigned j = 0; j < 52; ++j) {
        const unsigned char a = static_cast<unsigned char>((i < 26 ? 'A' : 'a') + i % 26);
        const unsigned char b = static_cast<unsigned char>((j < 26 ? 'A' : 'a') + j % 26);
        const int cmp = cs->coll->strnncollsp(cs, &a, 1, &b, 1, false);
        require((cmp == 0) == (i % 26 == j % 26), "actual ASCII weight equivalence changed");
      }
    }

    OriginalSet original;
    ObStopWordChecker checker;
    bool untouched = true;
    const ObFTWord sample(3, "the", meta(CS_TYPE_UTF8MB4_GENERAL_CI));
    require(checker.check_stopword(sample, untouched) == OB_NOT_INIT && untouched,
            "uninitialized output changed");
    require(checker.init() == OB_SUCCESS, "checker init");
    require(checker.init() == OB_INIT_TWICE, "init twice");
    const ObCollationType collations[] = {
      CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_UNICODE_CI,
      CS_TYPE_BINARY, CS_TYPE_LATIN1_SWEDISH_CI, CS_TYPE_UTF16_GENERAL_CI,
      CS_TYPE_UTF16LE_GENERAL_CI, CS_TYPE_GBK_CHINESE_CI, CS_TYPE_INVALID
    };
    for (const auto &builtin : ob_stop_word_list) {
      const std::string word(builtin);
      require(word.size() < 10, "review case-variant enumeration after list change");
      for (uint64_t mask = 0; mask < (uint64_t(1) << word.size()); ++mask) {
        std::string variant = word;
        for (size_t pos = 0; pos < word.size(); ++pos) {
          if (mask & (uint64_t(1) << pos)) {
            variant[pos] = static_cast<char>(cs->caseinfo->page[0][
                static_cast<unsigned char>(word[pos])].toupper);
          }
        }
        for (const auto coll : collations) {
          compare(checker, original, variant, coll);
          compare(checker, original, variant + "z", coll);
        }
      }
    }
    std::vector<std::string> boundaries{
      "", "the ", "the  ", " the", "the\t", "the9", "9the", "the_", "t_he",
      std::string("the\0", 4), std::string("th\0e", 4), std::string("the\x7f", 4),
      "th\xc3\xa9", "\xc3\xa1" "bout", "the\xcc\x81", "\xc4\xb0",
      "\xef\xbc\xb4\xef\xbc\xa8\xef\xbc\xa5",
      "\xed\xa0\x80", "\xc0\x80", "\xe0\x80\x80", "\xf4\x90\x80\x80",
      std::string(9, 'a'), std::string(10, 'a'), std::string(4097, 'a')
    };
    for (unsigned byte = 0; byte < 256; ++byte) {
      boundaries.push_back(std::string("the") + static_cast<char>(byte));
      boundaries.push_back(std::string(1, static_cast<char>(byte)) + "the");
      boundaries.push_back(std::string("th") + static_cast<char>(byte) + "e");
    }
    for (const auto coll : collations) {
      for (const auto &text : boundaries) {
        compare(checker, original, text, coll);
      }
      for (char a = 'a'; a <= 'z'; ++a) {
        for (char b = 'a'; b <= 'z'; ++b) {
          compare(checker, original, std::string{a, b}, coll);
        }
      }
    }
    checker.destroy();
    checker.destroy();
    untouched = false;
    require(checker.check_stopword(sample, untouched) == OB_NOT_INIT && !untouched,
            "destroyed output changed");
    require(checker.init() == OB_SUCCESS, "re-init after destroy");
    compare(checker, original, "ThE", CS_TYPE_UTF8MB4_BIN);
    compare(checker, original, "the ", CS_TYPE_UTF8MB4_GENERAL_CI);
    checker.destroy();
    std::cout << "PASS direct_checker_checks=" << checks << "\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << "\n";
    return 1;
  }
}
