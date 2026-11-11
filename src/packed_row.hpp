#ifndef MVD_PACKED_ROW_HPP
#define MVD_PACKED_ROW_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace mvd::packed_row {

enum class Type : std::uint8_t { BigInt = 1, VarChar = 2, Vector = 3 };

struct ColumnSpec {
  Type type;
  std::uint32_t length;
  bool primary;
};

class Cell;
class View;
class PackedRow;

// Layouts must outlive their views and must not change while those views live.
class Layout {
 public:
  Layout() = default;

  explicit Layout(std::span<const ColumnSpec> columns) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (columns.empty() || columns.size() > maximum) return;
    std::uint64_t offset = sizeof(std::uint32_t);
    bool found_primary = false;
    for (const ColumnSpec &column : columns) {
      std::uint32_t alignment;
      std::uint64_t width;
      switch (column.type) {
        case Type::BigInt:
          if (column.length != 0) return;
          alignment = 8;
          width = sizeof(std::int64_t);
          break;
        case Type::VarChar:
          if (column.length == 0 || column.primary) return;
          alignment = 4;
          width = sizeof(std::uint32_t);
          break;
        case Type::Vector:
          if (column.length == 0 || column.primary) return;
          alignment = 4;
          width = std::uint64_t{column.length} * sizeof(float);
          break;
        default:
          return;
      }
      if (column.primary) {
        if (found_primary) return;
        found_primary = true;
        slots_.push_back({column.type, column.length, 0, true});
        continue;
      }
      offset = (offset + alignment - 1) & ~(std::uint64_t{alignment} - 1);
      if (offset + width > maximum) return;
      slots_.push_back({column.type, column.length,
                        static_cast<std::uint32_t>(offset), false});
      offset += width;
    }
    if (!found_primary) return;
    fixed_bytes_ = static_cast<std::uint32_t>(offset);
    supported_ = true;
  }

  bool supported() const noexcept { return supported_; }
  std::size_t size() const noexcept { return supported_ ? slots_.size() : 0; }

 private:
  struct Slot {
    Type type;
    std::uint32_t length;
    std::uint32_t offset;
    bool primary;
  };

  std::vector<Slot> slots_;
  std::uint32_t fixed_bytes_ = 0;
  bool supported_ = false;

  friend class Cell;
  friend class View;
  friend class PackedRow;
};

class Cell {
 public:
  Cell() = default;

  std::int64_t integer() const noexcept {
    if (slot_ == nullptr || slot_->type != Type::BigInt) return 0;
    if (slot_->primary) return primary_;
    std::int64_t result;
    std::memcpy(&result, data_ + slot_->offset, sizeof(result));
    return result;
  }

  std::string_view text() const noexcept {
    if (slot_ == nullptr || slot_->type != Type::VarChar) return {};
    std::uint32_t offset;
    std::memcpy(&offset, data_ + slot_->offset, sizeof(offset));
    std::uint32_t length;
    std::memcpy(&length, data_ + offset, sizeof(length));
    return {reinterpret_cast<const char *>(data_ + offset + sizeof(length)),
            length};
  }

  std::span<const float> vector() const noexcept {
    if (slot_ == nullptr || slot_->type != Type::Vector) return {};
    return {reinterpret_cast<const float *>(data_ + slot_->offset),
            slot_->length};
  }

 private:
  Cell(const std::byte *data, const Layout::Slot *slot,
       std::int64_t primary) noexcept
      : data_(data), slot_(slot), primary_(primary) {}

  const std::byte *data_ = nullptr;
  const Layout::Slot *slot_ = nullptr;
  std::int64_t primary_ = 0;

  friend class View;
};

// Nonowning: the packed allocation and its original Layout must remain alive.
class View {
 public:
  View() = default;
  View(const std::byte *data, const Layout *layout, std::int64_t primary) noexcept
      : data_(data), layout_(layout), primary_(primary) {}

  const std::byte *data() const noexcept { return data_; }
  const Layout *layout() const noexcept { return layout_; }
  std::int64_t primary() const noexcept { return primary_; }
  std::size_t size() const noexcept {
    return data_ != nullptr && layout_ != nullptr ? layout_->size() : 0;
  }
  Cell operator[](std::size_t index) const noexcept {
    if (index >= size()) return {};
    return Cell(data_, &layout_->slots_[index], primary_);
  }

 private:
  const std::byte *data_ = nullptr;
  const Layout *layout_ = nullptr;
  std::int64_t primary_ = 0;
};

class PackedRow {
 public:
  PackedRow() = default;
  PackedRow(const PackedRow &) = delete;
  PackedRow &operator=(const PackedRow &) = delete;
  PackedRow(PackedRow &&) noexcept = default;
  PackedRow &operator=(PackedRow &&) noexcept = default;

  // Row supplies size(), operator[], and integer()/text()/vector() cell getters.
  // The caller validates SQL text lengths (Unicode codepoints). Only byte-count
  // overflow is checked here, so an oversized row can use its disk fallback.
  template <class Row>
  bool assign(const Layout &layout, const Row &row) {
    if (!layout.supported() || row.size() != layout.size()) return false;
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t total = layout.fixed_bytes_;
    for (std::size_t index = 0; index < layout.size(); ++index) {
      const auto &slot = layout.slots_[index];
      if (slot.type == Type::Vector &&
          row[index].vector().size() != slot.length) return false;
      if (slot.type == Type::VarChar) {
        const auto length = row[index].text().size();
        if (length > maximum) return false;
        total += sizeof(std::uint32_t) + static_cast<std::uint64_t>(length);
        if (total > maximum) return false;
      }
    }

    // byte[] provides float alignment; memcpy creates the float objects without
    // evaluating them, preserving every float32 bit pattern, including NaNs.
    static_assert(sizeof(float) == 4 && alignof(float) <= 4);
    std::unique_ptr<std::byte[]> staged(new std::byte[static_cast<std::size_t>(total)]);
    const auto byte_count = static_cast<std::uint32_t>(total);
    std::memcpy(staged.get(), &byte_count, sizeof(byte_count));
    std::uint32_t tail = layout.fixed_bytes_;
    std::uint32_t written = sizeof(byte_count);
    for (std::size_t index = 0; index < layout.size(); ++index) {
      const auto &slot = layout.slots_[index];
      if (slot.primary) continue;
      if (slot.offset > written)
        std::memset(staged.get() + written, 0, slot.offset - written);
      std::byte *destination = staged.get() + slot.offset;
      switch (slot.type) {
        case Type::BigInt: {
          const std::int64_t value = row[index].integer();
          std::memcpy(destination, &value, sizeof(value));
          written = slot.offset + sizeof(value);
          break;
        }
        case Type::VarChar: {
          const auto &text = row[index].text();
          const auto length = static_cast<std::uint32_t>(text.size());
          std::memcpy(destination, &tail, sizeof(tail));
          std::memcpy(staged.get() + tail, &length, sizeof(length));
          tail += sizeof(length);
          if (length != 0)
            std::memcpy(staged.get() + tail, text.data(), length);
          tail += length;
          written = slot.offset + sizeof(tail);
          break;
        }
        case Type::Vector: {
          const auto &vector = row[index].vector();
          const auto bytes = static_cast<std::size_t>(slot.length) * sizeof(float);
          std::memcpy(destination, vector.data(), bytes);
          written = slot.offset + static_cast<std::uint32_t>(bytes);
          break;
        }
      }
    }
    data_.swap(staged);
    return true;
  }

  std::size_t bytes() const noexcept {
    if (!data_) return 0;
    std::uint32_t result;
    std::memcpy(&result, data_.get(), sizeof(result));
    return result;
  }
  View view(const Layout &layout, std::int64_t primary) const noexcept {
    return View(data_.get(), &layout, primary);
  }
  void reset() noexcept { data_.reset(); }
  void swap(PackedRow &other) noexcept { data_.swap(other.data_); }

 private:
  std::unique_ptr<std::byte[]> data_;
};

inline void swap(PackedRow &left, PackedRow &right) noexcept { left.swap(right); }

}  // namespace mvd::packed_row

#endif
