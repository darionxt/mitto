// The parser of a multipart/form-data body that formData() reads, as Blink's
// MultipartParser: it takes the body in pieces and tells its client of each
// part's header fields, of the part's bytes as they come, and of each part's
// end. A part's header fields are those that //net would parse, of which the
// client hears Content-Disposition and Content-Type.
//
// Upstream: third_party/blink/renderer/core/fetch/multipart_parser.h
// Upstream: third_party/blink/renderer/core/fetch/multipart_parser.cc
// Upstream: third_party/blink/renderer/platform/network/http_parsers.cc
// Upstream: net/http/http_util.cc
// Upstream: net/http/http_response_headers.cc

#ifndef MITTO_DOM_MULTIPART_PARSER_H_
#define MITTO_DOM_MULTIPART_PARSER_H_

#include <stddef.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

// The header fields of a part that the client reads, each the first value
// that //net gives for its name, as Blink's HTTPHeaderMap::Add() keeps only
// the first: of the first line of the name, up to a comma outside quotes. A
// value that is not UTF-8 is null, as Blink's String::FromUtf8() gives it.
class MultipartPartHeaders {
 public:
  MultipartPartHeaders();
  MultipartPartHeaders(const MultipartPartHeaders&);
  MultipartPartHeaders& operator=(const MultipartPartHeaders&);
  ~MultipartPartHeaders();

  const std::optional<std::u16string>& content_disposition() const {
    return content_disposition_.value;
  }
  const std::optional<std::u16string>& content_type() const {
    return content_type_.value;
  }

  // Parses a part's header fields, with the empty line that ends them.
  static MultipartPartHeaders Parse(std::string_view text);

 private:
  struct Field {
    Field();
    Field(const Field&);
    Field& operator=(const Field&);
    ~Field();

    bool present = false;
    std::optional<std::u16string> value;

    void Add(std::optional<std::u16string> added);
  };

  Field content_disposition_;
  Field content_type_;
};

class MultipartParser final : public cppgc::GarbageCollected<MultipartParser> {
 public:
  class Client : public cppgc::GarbageCollectedMixin {
   public:
    virtual ~Client() = default;
    // The header fields of a part.
    virtual void PartHeaderFieldsInMultipartReceived(
        const MultipartPartHeaders& header_fields) = 0;
    // Some of a part's bytes, which are never empty, zero or more times for
    // each part.
    virtual void PartDataInMultipartReceived(base::span<const char> bytes) = 0;
    // The end of a part's bytes.
    virtual void PartDataInMultipartFullyReceived() = 0;

    void Trace(cppgc::Visitor* visitor) const override {}
  };

  MultipartParser(std::string boundary, Client* client);
  MultipartParser(const MultipartParser&) = delete;
  MultipartParser& operator=(const MultipartParser&) = delete;
  ~MultipartParser();

  // False if `bytes` do not keep to the grammar, or the client cancelled.
  bool AppendData(base::span<const char> bytes);
  void Cancel();
  // Whether the body ended after a close delimiter.
  bool Finish();

  bool IsCancelled() const { return state_ == State::kCancelled; }

  void Trace(cppgc::Visitor* visitor) const;

 private:
  class Matcher {
   public:
    Matcher();
    Matcher(base::span<const char> match_data, size_t num_matched_bytes);

    bool Match(char value) {
      if (value != match_data_[num_matched_bytes_]) {
        return false;
      }
      ++num_matched_bytes_;
      return true;
    }
    bool Match(base::span<const char> data);
    bool IsMatchComplete() const {
      return num_matched_bytes_ == match_data_.size();
    }
    size_t NumMatchedBytes() const { return num_matched_bytes_; }
    void SetNumMatchedBytes(size_t num_matched_bytes);
    base::span<const char> MatchedData() const {
      return match_data_.first(num_matched_bytes_);
    }

   private:
    base::span<const char> match_data_;
    size_t num_matched_bytes_ = 0u;
  };

  enum class State {
    kParsingPreamble,
    kParsingDelimiterSuffix,
    kParsingPartHeaderFields,
    kParsingPartOctets,
    kParsingDelimiterOrCloseDelimiterSuffix,
    kParsingCloseDelimiterSuffix,
    kParsingEpilogue,
    kCancelled,
    kFinished,
  };

  Matcher CloseDelimiterSuffixMatcher() const;
  Matcher DelimiterMatcher(size_t num_already_matched_bytes = 0u) const;
  Matcher DelimiterSuffixMatcher() const;

  void ParseDataAndDelimiter(base::span<const char>& bytes);
  void ParseDelimiter(base::span<const char>& bytes);
  std::optional<MultipartPartHeaders> ParseHeaderFields(
      base::span<const char>& bytes);
  void ParseTransportPadding(base::span<const char>& bytes) const;

  Matcher matcher_;
  // The start of a header block that comes in pieces.
  std::vector<char> buffered_header_bytes_;
  // What LocateEndOfAdditionalHeaders()'s scan carries from one piece to the
  // next, so that each byte is scanned once.
  char header_scan_last_c_ = '\n';
  bool header_scan_was_lf_ = true;
  const cppgc::Member<Client> client_;
  // "\r\n--" and the boundary.
  const std::string delimiter_;
  State state_ = State::kParsingPreamble;
};

}  // namespace mitto

#endif  // MITTO_DOM_MULTIPART_PARSER_H_
