// A range of bytes that a Range header asks for, as //net's HttpByteRange
// holds it, and the Fetch Standard's single-range parser, as //net's
// HttpUtil::ParseFetchSingleRange() runs it for a blob: URL's loader in
// Chrome's browser. The Page program has no //net, so this ports them.
//
// Upstream: net/http/http_byte_range.h
// Upstream: net/http/http_byte_range.cc
// Upstream: net/http/http_util.cc

#ifndef MITTO_DOM_HTTP_BYTE_RANGE_H_
#define MITTO_DOM_HTTP_BYTE_RANGE_H_

#include <stdint.h>

#include <optional>
#include <string_view>

namespace mitto {

class HttpByteRange {
 public:
  HttpByteRange();

  // first-last, first-, and the last `suffix_length` bytes.
  static HttpByteRange Bounded(int64_t first_byte_position,
                               int64_t last_byte_position);
  static HttpByteRange RightUnbounded(int64_t first_byte_position);
  static HttpByteRange Suffix(int64_t suffix_length);

  int64_t first_byte_position() const { return first_byte_position_; }
  int64_t last_byte_position() const { return last_byte_position_; }

  bool IsSuffixByteRange() const;
  bool HasFirstBytePosition() const;
  bool HasLastBytePosition() const;
  bool IsValid() const;
  // Makes the positions absolute for a body of `size` bytes, and returns
  // whether the range fits it.
  bool ComputeBounds(int64_t size);

 private:
  int64_t first_byte_position_;
  int64_t last_byte_position_;
  int64_t suffix_length_;
  bool has_computed_bounds_ = false;
};

// The range that a Range header's value names, if it is "bytes=" and one range
// in the Fetch Standard's grammar, with spaces and tabs around "=" and "-" if
// `allow_whitespace`.
std::optional<HttpByteRange> ParseFetchSingleRange(
    std::string_view range_header_value,
    bool allow_whitespace);

}  // namespace mitto

#endif  // MITTO_DOM_HTTP_BYTE_RANGE_H_
