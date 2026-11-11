#ifndef MVD_ROW_VIEW_HPP
#define MVD_ROW_VIEW_HPP

#include "compact_value.hpp"
#include "packed_row.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace mvd {

// A cell view owns no payload. Mutable/parser rows keep their existing Value
// objects; committed packed rows expose only their schema-selected bytes.
class CellView {
 public:
  CellView(const Value &value) noexcept : storage_(&value) {}
  CellView(packed_row::Cell value) noexcept : storage_(value) {}

  std::int64_t integer() const noexcept {
    if (const auto *value = std::get_if<const Value *>(&storage_)) {
      return (*value)->integer();
    }
    return std::get<packed_row::Cell>(storage_).integer();
  }
  std::string_view text() const noexcept {
    if (const auto *value = std::get_if<const Value *>(&storage_)) {
      return (*value)->text();
    }
    return std::get<packed_row::Cell>(storage_).text();
  }
  std::span<const float> vector() const noexcept {
    if (const auto *value = std::get_if<const Value *>(&storage_)) {
      return (*value)->vector();
    }
    return std::get<packed_row::Cell>(storage_).vector();
  }
  Value own() const {
    if (const auto *value = std::get_if<const Value *>(&storage_)) return **value;
    Value result;
    const auto components = vector();
    const auto string = text();
    if (!components.empty()) {
      result.setVector(std::vector<float>(components.begin(), components.end()));
    } else if (!string.empty()) {
      result.setText(std::string(string));
    } else {
      // All inactive fields are zero/empty, including an empty VARCHAR. This
      // preserves the unchanged row encoder's complete per-cell bytes.
      result.setInteger(integer());
    }
    return result;
  }

 private:
  std::variant<const Value *, packed_row::Cell> storage_;
};

// This small borrowed value can itself move with a heap candidate. Its payload
// survives until a committed mutation/cache invalidation, or until the caller
// releases a transient owning row. RetainedRow handles the latter explicitly.
class RowView {
 public:
  RowView() noexcept = default;
  RowView(const std::vector<Value> &row) noexcept
      : data_(row.data()), context_(static_cast<std::int64_t>(row.size())) {}
  RowView(packed_row::View row) noexcept
      : data_(row.data()), layout_(row.layout()), context_(row.primary()) {}

  bool valid() const noexcept { return data_ != nullptr; }
  bool isPacked() const noexcept { return layout_ != nullptr; }
  std::size_t size() const noexcept {
    return isPacked() ? packed().size() : static_cast<std::size_t>(context_);
  }
  bool empty() const noexcept { return size() == 0; }
  CellView operator[](std::size_t column) const noexcept {
    return isPacked() ? CellView(packed()[column])
                      : CellView(static_cast<const Value *>(data_)[column]);
  }
  std::vector<Value> materialize() const {
    std::vector<Value> row;
    row.reserve(size());
    for (std::size_t column = 0; column < size(); ++column) {
      row.push_back((*this)[column].own());
    }
    return row;
  }

 private:
  packed_row::View packed() const noexcept {
    return {static_cast<const std::byte *>(data_), layout_, context_};
  }
  const void *data_ = nullptr;
  const packed_row::Layout *layout_ = nullptr;
  std::int64_t context_ = 0;
};

}  // namespace mvd

#endif
