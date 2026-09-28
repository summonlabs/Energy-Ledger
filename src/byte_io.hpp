// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounds-checked little-endian byte readers and writers. Every multi-byte
// field in the durable format and in the canonical encoding goes through these
// helpers, so endianness is explicit and decode failures are always reported
// instead of read past the end of a buffer.

#ifndef ENERGY_LEDGER_SRC_BYTE_IO_HPP
#define ENERGY_LEDGER_SRC_BYTE_IO_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace energy_ledger {
namespace detail {

class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::uint8_t>& out) noexcept : out_(out) {}

  void u8(std::uint8_t value) { out_.push_back(value); }

  void u16(std::uint16_t value) {
    out_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  }

  void u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      out_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  void u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      out_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void bytes(const void* data, std::size_t length) {
    const auto* first = static_cast<const std::uint8_t*>(data);
    out_.insert(out_.end(), first, first + length);
  }

  /// Length-prefixed byte string. Refuses to encode more than 65535 bytes.
  bool sized_bytes(const void* data, std::size_t length) {
    if (length > 0xFFFFu) {
      return false;
    }
    u16(static_cast<std::uint16_t>(length));
    bytes(data, length);
    return true;
  }

  std::size_t size() const noexcept { return out_.size(); }

 private:
  std::vector<std::uint8_t>& out_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t length) noexcept
      : data_(data), length_(length) {}

  bool u8(std::uint8_t& value) {
    if (remaining() < 1) {
      return false;
    }
    value = data_[pos_++];
    return true;
  }

  bool u16(std::uint16_t& value) {
    if (remaining() < 2) {
      return false;
    }
    value = static_cast<std::uint16_t>(data_[pos_]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return true;
  }

  bool u32(std::uint32_t& value) {
    if (remaining() < 4) {
      return false;
    }
    std::uint32_t result = 0;
    for (int index = 0; index < 4; ++index) {
      result |= static_cast<std::uint32_t>(data_[pos_ + static_cast<std::size_t>(index)])
                << (8 * index);
    }
    pos_ += 4;
    value = result;
    return true;
  }

  bool u64(std::uint64_t& value) {
    if (remaining() < 8) {
      return false;
    }
    std::uint64_t result = 0;
    for (int index = 0; index < 8; ++index) {
      result |= static_cast<std::uint64_t>(data_[pos_ + static_cast<std::size_t>(index)])
                << (8 * index);
    }
    pos_ += 8;
    value = result;
    return true;
  }

  bool i64(std::int64_t& value) {
    std::uint64_t raw = 0;
    if (!u64(raw)) {
      return false;
    }
    value = static_cast<std::int64_t>(raw);
    return true;
  }

  bool bytes(void* destination, std::size_t length) {
    if (remaining() < length) {
      return false;
    }
    const auto* source = data_ + pos_;
    auto* target = static_cast<std::uint8_t*>(destination);
    for (std::size_t index = 0; index < length; ++index) {
      target[index] = source[index];
    }
    pos_ += length;
    return true;
  }

  bool skip(std::size_t length) {
    if (remaining() < length) {
      return false;
    }
    pos_ += length;
    return true;
  }

  /// Length-prefixed byte string with an explicit upper bound.
  bool sized_bytes(std::size_t max_length, const std::uint8_t*& out, std::size_t& out_length) {
    std::uint16_t length = 0;
    if (!u16(length)) {
      return false;
    }
    if (length > max_length || remaining() < length) {
      return false;
    }
    out = data_ + pos_;
    out_length = length;
    pos_ += length;
    return true;
  }

  std::size_t remaining() const noexcept { return length_ - pos_; }
  std::size_t offset() const noexcept { return pos_; }

 private:
  const std::uint8_t* data_;
  std::size_t length_;
  std::size_t pos_ = 0;
};

}  // namespace detail
}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_SRC_BYTE_IO_HPP
