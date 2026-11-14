// What the families of page messages that carry an operation byte write and
// read: little-endian numbers and sized strings, as the other messages have
// them. A Reader never reads past its payload, and never trusts a count
// further than the payload could hold.

#ifndef MITTO_MESSAGE_CODING_H_
#define MITTO_MESSAGE_CODING_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/numerics/byte_conversions.h"

namespace mitto {

class MessageWriter {
 public:
  void Byte(uint8_t value) { out_.push_back(static_cast<char>(value)); }
  void Bool(bool value) { Byte(value ? 1 : 0); }
  void U16(uint16_t value) { Append(base::U16ToLittleEndian(value)); }
  void U32(uint32_t value) { Append(base::U32ToLittleEndian(value)); }
  void U64(uint64_t value) { Append(base::U64ToLittleEndian(value)); }
  void I32(int32_t value) { U32(static_cast<uint32_t>(value)); }
  void I64(int64_t value) { U64(static_cast<uint64_t>(value)); }
  void Sized(std::string_view text) {
    U32(static_cast<uint32_t>(text.size()));
    out_.append(text);
  }
  // UTF-16 code units, which may be lone surrogates.
  void Sized16(std::u16string_view text) {
    U32(static_cast<uint32_t>(text.size()));
    for (char16_t unit : text) {
      U16(unit);
    }
  }
  void Raw(std::string_view bytes) { out_.append(bytes); }
  size_t size() const { return out_.size(); }
  std::string Take() { return std::move(out_); }

 private:
  template <size_t N>
  void Append(const std::array<uint8_t, N>& bytes) {
    out_.append(base::as_string_view(base::span(bytes)));
  }

  std::string out_;
};

class MessageReader {
 public:
  explicit MessageReader(std::string_view payload) : rest_(payload) {}

  bool Byte(uint8_t& value) {
    if (rest_.empty()) {
      return false;
    }
    value = static_cast<uint8_t>(rest_[0]);
    rest_.remove_prefix(1);
    return true;
  }
  bool Bool(bool& value) {
    uint8_t byte = 0;
    if (!Byte(byte) || byte > 1) {
      return false;
    }
    value = byte == 1;
    return true;
  }
  bool U16(uint16_t& value) {
    std::string_view bytes;
    if (!Take(2, bytes)) {
      return false;
    }
    value = base::U16FromLittleEndian(base::as_byte_span(bytes).first<2>());
    return true;
  }
  bool U32(uint32_t& value) {
    std::string_view bytes;
    if (!Take(4, bytes)) {
      return false;
    }
    value = base::U32FromLittleEndian(base::as_byte_span(bytes).first<4>());
    return true;
  }
  bool U64(uint64_t& value) {
    std::string_view bytes;
    if (!Take(8, bytes)) {
      return false;
    }
    value = base::U64FromLittleEndian(base::as_byte_span(bytes).first<8>());
    return true;
  }
  bool I32(int32_t& value) {
    uint32_t unsigned_value = 0;
    if (!U32(unsigned_value)) {
      return false;
    }
    value = static_cast<int32_t>(unsigned_value);
    return true;
  }
  bool I64(int64_t& value) {
    uint64_t unsigned_value = 0;
    if (!U64(unsigned_value)) {
      return false;
    }
    value = static_cast<int64_t>(unsigned_value);
    return true;
  }
  bool Sized(std::string& text) {
    uint32_t size = 0;
    std::string_view bytes;
    if (!U32(size) || !Take(size, bytes)) {
      return false;
    }
    text.assign(bytes);
    return true;
  }
  bool Sized16(std::u16string& text) {
    uint32_t size = 0;
    if (!U32(size) || size > rest_.size() / 2) {
      return false;
    }
    text.resize(size);
    for (char16_t& unit : text) {
      uint16_t value = 0;
      if (!U16(value)) {
        return false;
      }
      unit = value;
    }
    return true;
  }
  // A count of items, each at least `least` bytes, so that a count cannot
  // make the reader reserve more than the payload holds.
  bool Count(uint32_t& count, size_t least) {
    return U32(count) && count <= rest_.size() / least;
  }
  std::string_view Rest() {
    std::string_view rest = rest_;
    rest_ = {};
    return rest;
  }
  bool AtEnd() const { return rest_.empty(); }

 private:
  bool Take(size_t size, std::string_view& bytes) {
    if (rest_.size() < size) {
      return false;
    }
    bytes = rest_.substr(0, size);
    rest_.remove_prefix(size);
    return true;
  }

  std::string_view rest_;
};

}  // namespace mitto

#endif  // MITTO_MESSAGE_CODING_H_
