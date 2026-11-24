#include "mitto/dom/multipart_parser.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

#include "base/check_op.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

namespace mitto {
namespace {

constexpr char kCloseDelimiterSuffix[] = "--\r\n";
constexpr size_t kDashBoundaryOffset = 2u;  // The length of "\r\n".
constexpr char kDelimiterSuffix[] = "\r\n";

bool IsLws(char c) {
  return c == ' ' || c == '\t';
}

std::string_view TrimLws(std::string_view text) {
  while (!text.empty() && IsLws(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsLws(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// net's HttpUtil::IsToken(), of RFC 7230's token characters.
bool IsHttpToken(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x7F || u <= 0x20 || c == '(' || c == ')' || c == '<' ||
        c == '>' || c == '@' || c == ',' || c == ';' || c == ':' || c == '\\' ||
        c == '"' || c == '/' || c == '[' || c == ']' || c == '?' || c == '=' ||
        c == '{' || c == '}') {
      return false;
    }
  }
  return true;
}

// Whether a line can go on in the next, as net's IsLineSegmentContinuable():
// it has a name, which does not start with whitespace, and a colon.
bool IsLineSegmentContinuable(std::string_view line) {
  const size_t colon = line.find(':');
  return colon != std::string_view::npos && colon > 0 && !IsLws(line[0]);
}

// The lines of `headers` as net's AssembleRawHeaders() joins them: split at
// each run of CR and LF, with a line that starts with whitespace after one
// that can go on joined to it with a space, and with NUL bytes removed.
std::vector<std::string> AssembleLines(std::string_view headers) {
  std::vector<std::string> lines;
  bool previous_continuable = false;
  size_t i = 0;
  while (i < headers.size()) {
    while (i < headers.size() && (headers[i] == '\r' || headers[i] == '\n')) {
      ++i;
    }
    if (i == headers.size()) {
      break;
    }
    const size_t start = i;
    while (i < headers.size() && headers[i] != '\r' && headers[i] != '\n') {
      ++i;
    }
    const std::string_view line = headers.substr(start, i - start);
    if (previous_continuable && IsLws(line[0])) {
      std::string_view rest = line;
      while (!rest.empty() && IsLws(rest.front())) {
        rest.remove_prefix(1);
      }
      lines.back() += ' ';
      lines.back() += rest;
    } else {
      lines.emplace_back(line);
      previous_continuable = IsLineSegmentContinuable(line);
    }
  }
  for (std::string& line : lines) {
    std::erase(line, '\0');
  }
  return lines;
}

// The values of a field, split at commas outside quotes and trimmed, as
// net's HttpResponseHeaders::AddHeader() keeps those of a field that can
// coalesce, empty ones among them.
std::vector<std::string_view> SplitValues(std::string_view values) {
  if (values.empty()) {
    return {values};
  }
  std::vector<std::string_view> split;
  size_t i = 0;
  while (true) {
    const size_t start = i;
    bool in_quote = false;
    bool in_escape = false;
    while (i < values.size()) {
      const char c = values[i];
      if (in_quote) {
        if (in_escape) {
          in_escape = false;
        } else if (c == '\\') {
          in_escape = true;
        } else if (c == '"') {
          in_quote = false;
        }
      } else if (c == ',') {
        break;
      } else if (c == '"') {
        in_quote = true;
      }
      ++i;
    }
    split.push_back(TrimLws(values.substr(start, i - start)));
    if (i == values.size()) {
      break;
    }
    ++i;
  }
  return split;
}

}  // namespace

MultipartPartHeaders::MultipartPartHeaders() = default;
MultipartPartHeaders::MultipartPartHeaders(const MultipartPartHeaders&) =
    default;
MultipartPartHeaders& MultipartPartHeaders::operator=(
    const MultipartPartHeaders&) = default;
MultipartPartHeaders::~MultipartPartHeaders() = default;

MultipartPartHeaders::Field::Field() = default;
MultipartPartHeaders::Field::Field(const Field&) = default;
MultipartPartHeaders::Field& MultipartPartHeaders::Field::operator=(
    const Field&) = default;
MultipartPartHeaders::Field::~Field() = default;

void MultipartPartHeaders::Field::Add(std::optional<std::u16string> added) {
  if (!present) {
    present = true;
    value = std::move(added);
  }
}

// static
MultipartPartHeaders MultipartPartHeaders::Parse(std::string_view text) {
  MultipartPartHeaders headers;
  for (const std::string& line : AssembleLines(text)) {
    // net's HeadersIterator: a name that is a token, before a colon.
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0 || IsLws(line[0])) {
      continue;
    }
    const std::string_view name =
        TrimLws(std::string_view(line).substr(0, colon));
    if (!IsHttpToken(name)) {
      continue;
    }
    Field* field = nullptr;
    if (base::EqualsCaseInsensitiveASCII(name, "content-disposition")) {
      field = &headers.content_disposition_;
    } else if (base::EqualsCaseInsensitiveASCII(name, "content-type")) {
      field = &headers.content_type_;
    } else {
      continue;
    }
    const std::string_view values =
        TrimLws(std::string_view(line).substr(colon + 1));
    for (const std::string_view value : SplitValues(values)) {
      std::u16string decoded;
      if (base::UTF8ToUTF16(value.data(), value.size(), &decoded)) {
        field->Add(std::move(decoded));
      } else {
        field->Add(std::nullopt);
      }
    }
  }
  return headers;
}

MultipartParser::Matcher::Matcher() = default;

MultipartParser::Matcher::Matcher(base::span<const char> match_data,
                                  size_t num_matched_bytes)
    : match_data_(match_data), num_matched_bytes_(num_matched_bytes) {}

bool MultipartParser::Matcher::Match(base::span<const char> data) {
  for (const char c : data) {
    if (!Match(c)) {
      return false;
    }
  }
  return true;
}

void MultipartParser::Matcher::SetNumMatchedBytes(size_t num_matched_bytes) {
  DCHECK_LE(num_matched_bytes, match_data_.size());
  num_matched_bytes_ = num_matched_bytes;
}

MultipartParser::MultipartParser(std::string boundary, Client* client)
    : client_(client), delimiter_("\r\n--" + std::move(boundary)) {
  matcher_ = DelimiterMatcher(kDashBoundaryOffset);
}

MultipartParser::~MultipartParser() = default;

bool MultipartParser::AppendData(base::span<const char> bytes) {
  DCHECK_NE(State::kFinished, state_);
  DCHECK_NE(State::kCancelled, state_);

  while (!bytes.empty()) {
    switch (state_) {
      case State::kParsingPreamble:
        // Either a preamble and a delimiter, or a dash boundary.
        ParseDelimiter(bytes);
        if (!matcher_.IsMatchComplete() && !bytes.empty()) {
          // A preamble, which goes, and then a delimiter.
          matcher_.SetNumMatchedBytes(0u);
          ParseDataAndDelimiter(bytes);
        }
        if (matcher_.IsMatchComplete()) {
          state_ = State::kParsingDelimiterOrCloseDelimiterSuffix;
        }
        break;

      case State::kParsingDelimiterSuffix:
        // Transport padding and "\r\n" after a delimiter.
        if (matcher_.NumMatchedBytes() == 0u) {
          ParseTransportPadding(bytes);
        }
        while (!bytes.empty()) {
          if (!matcher_.Match(bytes.front())) {
            return false;
          }
          bytes = bytes.subspan(1u);
          if (matcher_.IsMatchComplete()) {
            state_ = State::kParsingPartHeaderFields;
            break;
          }
        }
        break;

      case State::kParsingPartHeaderFields: {
        // A part's header fields and the empty line after them.
        std::optional<MultipartPartHeaders> header_fields =
            ParseHeaderFields(bytes);
        if (header_fields) {
          matcher_ = DelimiterMatcher();
          state_ = State::kParsingPartOctets;
          client_->PartHeaderFieldsInMultipartReceived(*header_fields);
        }
        break;
      }

      case State::kParsingPartOctets: {
        // A part's bytes and a delimiter.
        const size_t num_initially_matched_bytes = matcher_.NumMatchedBytes();
        base::span<const char> bytes_before = bytes;
        ParseDelimiter(bytes);
        if (!matcher_.IsMatchComplete() && !bytes.empty()) {
          if (matcher_.NumMatchedBytes() >= num_initially_matched_bytes &&
              num_initially_matched_bytes > 0u) {
            // The matched bytes, some from the previous call, formed no
            // delimiter, so they are the part's bytes.
            client_->PartDataInMultipartReceived(matcher_.MatchedData());
            if (state_ != State::kParsingPartOctets) {
              break;
            }
            bytes_before = bytes;
          }
          matcher_.SetNumMatchedBytes(0u);
          ParseDataAndDelimiter(bytes);

          const size_t skipped_size = bytes_before.size() - bytes.size();
          if (skipped_size > matcher_.NumMatchedBytes()) {
            const size_t payload_size =
                skipped_size - matcher_.NumMatchedBytes();
            client_->PartDataInMultipartReceived(
                bytes_before.first(payload_size));
            if (state_ != State::kParsingPartOctets) {
              break;
            }
          }
        }
        if (matcher_.IsMatchComplete()) {
          state_ = State::kParsingDelimiterOrCloseDelimiterSuffix;
          client_->PartDataInMultipartFullyReceived();
        }
        break;
      }

      case State::kParsingDelimiterOrCloseDelimiterSuffix:
        if (bytes.front() == '-') {
          matcher_ = CloseDelimiterSuffixMatcher();
          state_ = State::kParsingCloseDelimiterSuffix;
        } else {
          matcher_ = DelimiterSuffixMatcher();
          state_ = State::kParsingDelimiterSuffix;
        }
        break;

      case State::kParsingCloseDelimiterSuffix:
        // "--", transport padding and "\r\n" after a delimiter.
        for (;;) {
          if (matcher_.NumMatchedBytes() == 2u) {
            ParseTransportPadding(bytes);
          }
          if (bytes.empty()) {
            break;
          }
          if (!matcher_.Match(bytes.front())) {
            return false;
          }
          bytes = bytes.subspan(1u);
          if (matcher_.IsMatchComplete()) {
            state_ = State::kParsingEpilogue;
            break;
          }
        }
        break;

      case State::kParsingEpilogue:
        // An epilogue, which goes.
        return true;

      case State::kCancelled:
      case State::kFinished:
        // The client changed the state.
        return false;
    }
  }
  return true;
}

void MultipartParser::Cancel() {
  state_ = State::kCancelled;
}

bool MultipartParser::Finish() {
  DCHECK_NE(State::kCancelled, state_);
  DCHECK_NE(State::kFinished, state_);
  const State initial_state = state_;
  state_ = State::kFinished;
  switch (initial_state) {
    case State::kParsingPartOctets:
      if (matcher_.NumMatchedBytes() > 0u) {
        // The matched bytes formed no delimiter, so they are the part's.
        client_->PartDataInMultipartReceived(matcher_.MatchedData());
      }
      return false;
    case State::kParsingCloseDelimiterSuffix:
      // A whole close delimiter, with or without the "\r\n" after it.
      return matcher_.NumMatchedBytes() >= 2u;
    case State::kParsingEpilogue:
      return true;
    default:
      return false;
  }
}

void MultipartParser::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(client_);
}

MultipartParser::Matcher MultipartParser::CloseDelimiterSuffixMatcher() const {
  return Matcher(base::span_from_cstring(kCloseDelimiterSuffix), 0u);
}

MultipartParser::Matcher MultipartParser::DelimiterMatcher(
    size_t num_already_matched_bytes) const {
  return Matcher(delimiter_, num_already_matched_bytes);
}

MultipartParser::Matcher MultipartParser::DelimiterSuffixMatcher() const {
  return Matcher(base::span_from_cstring(kDelimiterSuffix), 0u);
}

void MultipartParser::ParseDataAndDelimiter(base::span<const char>& bytes) {
  DCHECK_EQ(0u, matcher_.NumMatchedBytes());
  const std::string_view view(bytes.data(), bytes.size());
  const size_t found = view.find(delimiter_);
  if (found != std::string_view::npos) {
    // A whole delimiter, with the part's bytes before it.
    bytes = bytes.subspan(found);
    const bool matched = matcher_.Match(bytes.first(delimiter_.size()));
    DCHECK(matched);
    DCHECK(matcher_.IsMatchComplete());
    bytes = bytes.subspan(delimiter_.size());
    return;
  }
  // The start of a delimiter at the end of the bytes, held for now; every
  // byte before it is the part's.
  base::span<const char> maybe_delimiter =
      bytes.last(std::min(delimiter_.size() - 1u, bytes.size()));
  while (!maybe_delimiter.empty()) {
    if (matcher_.Match(maybe_delimiter)) {
      break;
    }
    maybe_delimiter = maybe_delimiter.subspan(1u);
    matcher_.SetNumMatchedBytes(0u);
  }
  bytes = {};
}

void MultipartParser::ParseDelimiter(base::span<const char>& bytes) {
  DCHECK(!matcher_.IsMatchComplete());
  size_t matched = 0;
  while (matched < bytes.size() && matcher_.Match(bytes[matched])) {
    ++matched;
    if (matcher_.IsMatchComplete()) {
      break;
    }
  }
  bytes = bytes.subspan(matched);
}

std::optional<MultipartPartHeaders> MultipartParser::ParseHeaderFields(
    base::span<const char>& bytes) {
  // Blink's limit, which a chunk past it fails rather than waits on, as the
  // loop of AppendData() would offer it again.
  if (bytes.size() > std::numeric_limits<uint32_t>::max()) {
    state_ = State::kCancelled;
    return std::nullopt;
  }
  // The scan of net's LocateEndOfAdditionalHeaders(), over the new bytes: the
  // end of the empty line that ends the header fields.
  size_t scanned = 0;
  bool found_end = false;
  while (scanned < bytes.size() && !found_end) {
    const char c = bytes[scanned++];
    if (c == '\n') {
      found_end = header_scan_was_lf_;
      header_scan_was_lf_ = true;
    } else if (c != '\r' || header_scan_last_c_ != '\n') {
      header_scan_was_lf_ = false;
    }
    header_scan_last_c_ = c;
  }
  base::span<const char> header_bytes = bytes.first(scanned);
  bytes = bytes.subspan(scanned);
  // The header fields alone are held, and not the part's bytes after them.
  if (!found_end || !buffered_header_bytes_.empty()) {
    buffered_header_bytes_.insert(buffered_header_bytes_.end(),
                                  header_bytes.begin(), header_bytes.end());
    header_bytes = buffered_header_bytes_;
  }
  if (!found_end) {
    return std::nullopt;
  }
  header_scan_last_c_ = '\n';
  header_scan_was_lf_ = true;
  const MultipartPartHeaders header_fields = MultipartPartHeaders::Parse(
      std::string_view(header_bytes.data(), header_bytes.size()));
  buffered_header_bytes_.clear();
  return header_fields;
}

void MultipartParser::ParseTransportPadding(
    base::span<const char>& bytes) const {
  size_t matched = 0;
  while (matched < bytes.size() &&
         (bytes[matched] == '\t' || bytes[matched] == ' ')) {
    ++matched;
  }
  bytes = bytes.subspan(matched);
}

}  // namespace mitto
