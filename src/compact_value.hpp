#ifndef MVD_COMPACT_VALUE_HPP
#define MVD_COMPACT_VALUE_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mvd {

// Only one SQL type is live in a cell. Avoid retaining an unused string and
// vector object alongside every integer, string and vector in the row cache.
// The inactive accessors preserve the zero/empty fields of the row encoding.
class Value {
 public:
  std::int64_t integer() const noexcept {
    const auto *value = std::get_if<std::int64_t>(&storage_);
    return value == nullptr ? 0 : *value;
  }

  const std::string &text() const noexcept {
    const auto *value = std::get_if<std::string>(&storage_);
    return value == nullptr ? empty_text_ : *value;
  }

  const std::vector<float> &vector() const noexcept {
    const auto *value = std::get_if<std::vector<float>>(&storage_);
    return value == nullptr ? empty_vector_ : *value;
  }

  void setInteger(std::int64_t value) noexcept {
    storage_.emplace<std::int64_t>(value);
  }

  void setText(std::string value) noexcept {
    storage_.emplace<std::string>(std::move(value));
  }

  void setVector(std::vector<float> value) noexcept {
    storage_.emplace<std::vector<float>>(std::move(value));
  }

 private:
  std::variant<std::int64_t, std::string, std::vector<float>> storage_{};
  inline static const std::string empty_text_{};
  inline static const std::vector<float> empty_vector_{};
};

}  // namespace mvd

#endif
