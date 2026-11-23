#include "mitto/dom/http_byte_range.h"

#include <algorithm>
#include <limits>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace mitto {
namespace {

constexpr int64_t kPositionNotSpecified = -1;
constexpr std::string_view kBytesUnit = "bytes";

}  // namespace

HttpByteRange::HttpByteRange()
    : first_byte_position_(kPositionNotSpecified),
      last_byte_position_(kPositionNotSpecified),
      suffix_length_(kPositionNotSpecified) {}

// static
HttpByteRange HttpByteRange::Bounded(int64_t first_byte_position,
                                     int64_t last_byte_position) {
  HttpByteRange range;
  range.first_byte_position_ = first_byte_position;
  range.last_byte_position_ = last_byte_position;
  return range;
}

// static
HttpByteRange HttpByteRange::RightUnbounded(int64_t first_byte_position) {
  HttpByteRange range;
  range.first_byte_position_ = first_byte_position;
  return range;
}

// static
HttpByteRange HttpByteRange::Suffix(int64_t suffix_length) {
  HttpByteRange range;
  range.suffix_length_ = suffix_length;
  return range;
}

bool HttpByteRange::IsSuffixByteRange() const {
  return suffix_length_ != kPositionNotSpecified;
}

bool HttpByteRange::HasFirstBytePosition() const {
  return first_byte_position_ != kPositionNotSpecified;
}

bool HttpByteRange::HasLastBytePosition() const {
  return last_byte_position_ != kPositionNotSpecified;
}

bool HttpByteRange::IsValid() const {
  if (suffix_length_ > 0) {
    return true;
  }
  return first_byte_position_ >= 0 &&
         (last_byte_position_ == kPositionNotSpecified ||
          last_byte_position_ >= first_byte_position_);
}

bool HttpByteRange::ComputeBounds(int64_t size) {
  if (size < 0 || has_computed_bounds_) {
    return false;
  }
  has_computed_bounds_ = true;
  // Empty values.
  if (!HasFirstBytePosition() && !HasLastBytePosition() &&
      !IsSuffixByteRange()) {
    first_byte_position_ = 0;
    last_byte_position_ = size - 1;
    return true;
  }
  if (!IsValid()) {
    return false;
  }
  if (IsSuffixByteRange()) {
    first_byte_position_ = size - std::min(size, suffix_length_);
    last_byte_position_ = size - 1;
    return true;
  }
  if (first_byte_position_ < size) {
    last_byte_position_ = HasLastBytePosition()
                              ? std::min(size - 1, last_byte_position_)
                              : size - 1;
    return true;
  }
  return false;
}

std::optional<HttpByteRange> ParseFetchSingleRange(
    std::string_view range_header_value,
    bool allow_whitespace) {
  // Fetch requires the range unit to be exactly "bytes".
  if (!range_header_value.starts_with(kBytesUnit)) {
    return std::nullopt;
  }
  size_t pos = kBytesUnit.size();
  const auto in_bounds = [&] { return pos < range_header_value.size(); };
  const auto skip_optional_whitespace = [&] {
    if (!allow_whitespace) {
      return;
    }
    while (in_bounds() && (range_header_value[pos] == ' ' ||
                           range_header_value[pos] == '\t')) {
      ++pos;
    }
  };
  // Optional whitespace, `delimiter`, then optional whitespace.
  const auto consume_delimiter = [&](char delimiter) {
    skip_optional_whitespace();
    if (!in_bounds() || range_header_value[pos] != delimiter) {
      return false;
    }
    ++pos;
    skip_optional_whitespace();
    return true;
  };
  // A value too large for int64_t saturates to its maximum.
  const auto parse_decimal_number = [&]() -> std::optional<int64_t> {
    const size_t start = pos;
    while (in_bounds() && base::IsAsciiDigit(range_header_value[pos])) {
      ++pos;
    }
    if (pos == start) {
      return std::nullopt;
    }
    int64_t parsed = 0;
    if (!base::StringToInt64(range_header_value.substr(start, pos - start),
                             &parsed)) {
      return std::numeric_limits<int64_t>::max();
    }
    return parsed;
  };
  if (!consume_delimiter('=')) {
    return std::nullopt;
  }
  const std::optional<int64_t> range_start = parse_decimal_number();
  if (!consume_delimiter('-')) {
    return std::nullopt;
  }
  const std::optional<int64_t> range_end = parse_decimal_number();
  // Trailing input and an empty range fail.
  if (in_bounds() || (!range_start && !range_end)) {
    return std::nullopt;
  }
  if (!range_start) {
    return HttpByteRange::Suffix(*range_end);
  }
  if (!range_end) {
    return HttpByteRange::RightUnbounded(*range_start);
  }
  return HttpByteRange::Bounded(*range_start, *range_end);
}

}  // namespace mitto
