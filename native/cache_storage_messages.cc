#include "mitto/cache_storage_messages.h"

#include <array>
#include <limits>
#include <utility>

#include "base/containers/span.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/string_util.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"

namespace mitto {
namespace {

// How many bytes of a body one kBodyPart or kBodyData carries at most.
constexpr size_t kMaxBodyPartSize = 1024 * 1024;

// blink.mojom.CacheStorageError's largest value, and FetchResponseType's.
constexpr uint8_t kMaxCacheStorageError = 9;
constexpr uint8_t kMaxResponseType = 5;

// Little-endian numbers and sized strings, as the other messages write them.
class Writer {
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
  std::string Take() { return std::move(out_); }

 private:
  template <size_t N>
  void Append(const std::array<uint8_t, N>& bytes) {
    out_.append(base::as_string_view(base::span(bytes)));
  }

  std::string out_;
};

class Reader {
 public:
  explicit Reader(std::string_view payload) : rest_(payload) {}

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

// A header's name is a token and its value has no line breaks or NUL, as
// FetchHeaderList holds them.
bool IsHeaderName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (char c : name) {
    if (!base::IsAsciiAlphaNumeric(c) &&
        std::string_view("!#$%&'*+-.^_`|~").find(c) == std::string_view::npos) {
      return false;
    }
  }
  return true;
}

bool IsHeaderValue(std::string_view value) {
  return value.find_first_of(std::string_view("\r\n\0", 3)) ==
         std::string_view::npos;
}

bool IsUrl(const std::string& url) {
  return base::IsStringUTF8(url) && GURL(url).is_valid();
}

void WriteOptions(Writer& writer, const PageCacheQueryOptions& options) {
  writer.Byte(static_cast<uint8_t>((options.ignore_search ? 1 : 0) |
                                   (options.ignore_method ? 2 : 0) |
                                   (options.ignore_vary ? 4 : 0)));
}

bool ReadOptions(Reader& reader, PageCacheQueryOptions& options) {
  uint8_t flags = 0;
  if (!reader.Byte(flags) || flags > 7) {
    return false;
  }
  options.ignore_search = flags & 1;
  options.ignore_method = flags & 2;
  options.ignore_vary = flags & 4;
  return true;
}

void WriteHeaders(Writer& writer,
                  const std::vector<std::pair<std::string, std::string>>& list) {
  writer.U32(static_cast<uint32_t>(list.size()));
  for (const auto& [name, value] : list) {
    writer.Sized(name);
    writer.Sized(value);
  }
}

bool ReadHeaders(Reader& reader,
                 std::vector<std::pair<std::string, std::string>>& list) {
  uint32_t count = 0;
  if (!reader.Count(count, 9)) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    std::string name;
    std::string value;
    if (!reader.Sized(name) || !reader.Sized(value) || !IsHeaderName(name) ||
        !IsHeaderValue(value)) {
      return false;
    }
    list.emplace_back(std::move(name), std::move(value));
  }
  return true;
}

void WriteRequest(Writer& writer, const PageCacheRequest& request) {
  writer.Sized(request.url);
  writer.Sized(request.method);
  WriteHeaders(writer, request.headers);
}

bool ReadRequest(Reader& reader, PageCacheRequest& request) {
  return reader.Sized(request.url) && IsUrl(request.url) &&
         reader.Sized(request.method) &&
         (request.method.empty() || IsHeaderName(request.method)) &&
         ReadHeaders(reader, request.headers);
}

void WriteResponse(Writer& writer, const PageCacheResponse& response) {
  writer.U32(static_cast<uint32_t>(response.url_list.size()));
  for (const std::string& url : response.url_list) {
    writer.Sized(url);
  }
  writer.U16(response.status);
  writer.Sized(response.status_text);
  writer.Byte(response.type);
  WriteHeaders(writer, response.headers);
  writer.U32(static_cast<uint32_t>(response.cors_exposed_header_names.size()));
  for (const std::string& name : response.cors_exposed_header_names) {
    writer.Sized(name);
  }
  writer.I64(response.response_time);
  writer.U64(response.body_size);
  writer.U64(response.body);
}

bool ReadResponse(Reader& reader, PageCacheResponse& response) {
  uint32_t urls = 0;
  if (!reader.Count(urls, 4)) {
    return false;
  }
  for (uint32_t i = 0; i < urls; ++i) {
    std::string url;
    if (!reader.Sized(url) || !IsUrl(url)) {
      return false;
    }
    response.url_list.push_back(std::move(url));
  }
  uint32_t names = 0;
  if (!reader.U16(response.status) || !reader.Sized(response.status_text) ||
      !IsHeaderValue(response.status_text) || !reader.Byte(response.type) ||
      response.type > kMaxResponseType ||
      !ReadHeaders(reader, response.headers) || !reader.Count(names, 4)) {
    return false;
  }
  for (uint32_t i = 0; i < names; ++i) {
    std::string name;
    if (!reader.Sized(name) || !IsHeaderName(name)) {
      return false;
    }
    response.cors_exposed_header_names.push_back(std::move(name));
  }
  return reader.I64(response.response_time) &&
         reader.U64(response.body_size) && reader.U64(response.body) &&
         // A body has a number exactly when it has bytes.
         (response.body_size == 0) == (response.body == 0);
}

}  // namespace

PageCacheRequest::PageCacheRequest() = default;
PageCacheRequest::PageCacheRequest(const PageCacheRequest&) = default;
PageCacheRequest::PageCacheRequest(PageCacheRequest&&) = default;
PageCacheRequest& PageCacheRequest::operator=(const PageCacheRequest&) =
    default;
PageCacheRequest& PageCacheRequest::operator=(PageCacheRequest&&) = default;
PageCacheRequest::~PageCacheRequest() = default;

PageCacheResponse::PageCacheResponse() = default;
PageCacheResponse::PageCacheResponse(const PageCacheResponse&) = default;
PageCacheResponse::PageCacheResponse(PageCacheResponse&&) = default;
PageCacheResponse& PageCacheResponse::operator=(const PageCacheResponse&) =
    default;
PageCacheResponse& PageCacheResponse::operator=(PageCacheResponse&&) = default;
PageCacheResponse::~PageCacheResponse() = default;

PageCacheOperation::PageCacheOperation() = default;
PageCacheOperation::PageCacheOperation(const PageCacheOperation&) = default;
PageCacheOperation::PageCacheOperation(PageCacheOperation&&) = default;
PageCacheOperation& PageCacheOperation::operator=(const PageCacheOperation&) =
    default;
PageCacheOperation& PageCacheOperation::operator=(PageCacheOperation&&) =
    default;
PageCacheOperation::~PageCacheOperation() = default;

PageCacheStorageCall::PageCacheStorageCall() = default;
PageCacheStorageCall::PageCacheStorageCall(PageCacheStorageCall&&) = default;
PageCacheStorageCall& PageCacheStorageCall::operator=(PageCacheStorageCall&&) =
    default;
PageCacheStorageCall::~PageCacheStorageCall() = default;

PageCacheStorageAnswer::PageCacheStorageAnswer() = default;
PageCacheStorageAnswer::PageCacheStorageAnswer(PageCacheStorageAnswer&&) =
    default;
PageCacheStorageAnswer& PageCacheStorageAnswer::operator=(
    PageCacheStorageAnswer&&) = default;
PageCacheStorageAnswer::~PageCacheStorageAnswer() = default;

std::string EncodeCacheStorageCall(const PageCacheStorageCall& call) {
  Writer writer;
  writer.Byte(static_cast<uint8_t>(call.op));
  switch (call.op) {
    case CacheStorageCallOp::kOpen:
    case CacheStorageCallOp::kHas:
    case CacheStorageCallOp::kDelete:
      writer.U64(call.question);
      writer.Sized16(call.cache_name);
      break;
    case CacheStorageCallOp::kKeys:
    case CacheStorageCallOp::kEstimate:
      writer.U64(call.question);
      break;
    case CacheStorageCallOp::kMatch:
      writer.U64(call.question);
      WriteRequest(writer, *call.request);
      WriteOptions(writer, call.options);
      writer.Bool(call.has_cache_name);
      if (call.has_cache_name) {
        writer.Sized16(call.cache_name);
      }
      break;
    case CacheStorageCallOp::kCacheMatch:
      writer.U64(call.question);
      writer.U64(call.cache);
      WriteRequest(writer, *call.request);
      WriteOptions(writer, call.options);
      break;
    case CacheStorageCallOp::kCacheMatchAll:
    case CacheStorageCallOp::kCacheKeys:
      writer.U64(call.question);
      writer.U64(call.cache);
      writer.Bool(call.request.has_value());
      if (call.request) {
        WriteRequest(writer, *call.request);
      }
      WriteOptions(writer, call.options);
      break;
    case CacheStorageCallOp::kCacheBatch:
      writer.U64(call.question);
      writer.U64(call.cache);
      writer.U32(static_cast<uint32_t>(call.operations.size()));
      for (const PageCacheOperation& operation : call.operations) {
        writer.Bool(operation.put);
        WriteRequest(writer, operation.request);
        if (operation.put) {
          WriteResponse(writer, operation.response);
        } else {
          WriteOptions(writer, operation.options);
        }
      }
      break;
    case CacheStorageCallOp::kBodyPart:
      writer.U64(call.body);
      writer.Raw(call.bytes);
      break;
    case CacheStorageCallOp::kReadBody:
    case CacheStorageCallOp::kReleaseBody:
      writer.U64(call.body);
      break;
    case CacheStorageCallOp::kBodyCredit:
      writer.U64(call.body);
      writer.U32(call.credit);
      break;
    case CacheStorageCallOp::kReleaseCache:
      writer.U64(call.cache);
      break;
  }
  return writer.Take();
}

bool CacheStorageCallFits(const PageCacheStorageCall& call) {
  return EncodeCacheStorageCall(call).size() <= kMaxPayloadSize;
}

base::expected<PageCacheStorageCall, std::string> ReadCacheStorageCall(
    std::string_view payload) {
  Reader reader(payload);
  PageCacheStorageCall call;
  uint8_t op = 0;
  if (!reader.Byte(op) || op == 0 ||
      op > static_cast<uint8_t>(CacheStorageCallOp::kMaxValue)) {
    return base::unexpected("a Cache API call of an unknown operation");
  }
  call.op = static_cast<CacheStorageCallOp>(op);
  bool valid = false;
  switch (call.op) {
    case CacheStorageCallOp::kOpen:
    case CacheStorageCallOp::kHas:
    case CacheStorageCallOp::kDelete:
      valid = reader.U64(call.question) && reader.Sized16(call.cache_name);
      break;
    case CacheStorageCallOp::kKeys:
    case CacheStorageCallOp::kEstimate:
      valid = reader.U64(call.question);
      break;
    case CacheStorageCallOp::kMatch:
      call.request.emplace();
      valid = reader.U64(call.question) && ReadRequest(reader, *call.request) &&
              ReadOptions(reader, call.options) &&
              reader.Bool(call.has_cache_name) &&
              (!call.has_cache_name || reader.Sized16(call.cache_name));
      break;
    case CacheStorageCallOp::kCacheMatch:
      call.request.emplace();
      valid = reader.U64(call.question) && reader.U64(call.cache) &&
              ReadRequest(reader, *call.request) &&
              ReadOptions(reader, call.options);
      break;
    case CacheStorageCallOp::kCacheMatchAll:
    case CacheStorageCallOp::kCacheKeys: {
      bool has_request = false;
      valid = reader.U64(call.question) && reader.U64(call.cache) &&
              reader.Bool(has_request);
      if (valid && has_request) {
        call.request.emplace();
        valid = ReadRequest(reader, *call.request);
      }
      valid = valid && ReadOptions(reader, call.options);
      break;
    }
    case CacheStorageCallOp::kCacheBatch: {
      uint32_t count = 0;
      valid = reader.U64(call.question) && reader.U64(call.cache) &&
              reader.Count(count, 14) && count > 0;
      for (uint32_t i = 0; valid && i < count; ++i) {
        PageCacheOperation& operation = call.operations.emplace_back();
        valid = reader.Bool(operation.put) &&
                ReadRequest(reader, operation.request) &&
                (operation.put ? ReadResponse(reader, operation.response)
                               : ReadOptions(reader, operation.options));
      }
      break;
    }
    case CacheStorageCallOp::kBodyPart:
      valid = reader.U64(call.body);
      call.bytes.assign(reader.Rest());
      valid = valid && !call.bytes.empty() &&
              call.bytes.size() <= kMaxBodyPartSize;
      break;
    case CacheStorageCallOp::kReadBody:
    case CacheStorageCallOp::kReleaseBody:
      valid = reader.U64(call.body);
      break;
    case CacheStorageCallOp::kBodyCredit:
      valid = reader.U64(call.body) && reader.U32(call.credit) &&
              call.credit > 0;
      break;
    case CacheStorageCallOp::kReleaseCache:
      valid = reader.U64(call.cache);
      break;
  }
  if (!valid || !reader.AtEnd()) {
    return base::unexpected("a Cache API call that is not valid");
  }
  return call;
}

std::string EncodeCacheStorageAnswer(const PageCacheStorageAnswer& answer) {
  Writer writer;
  writer.Byte(static_cast<uint8_t>(answer.op));
  switch (answer.op) {
    case CacheStorageAnswerOp::kOpened:
      writer.U64(answer.question);
      writer.Byte(answer.error);
      writer.U64(answer.cache);
      break;
    case CacheStorageAnswerOp::kBoolean:
      writer.U64(answer.question);
      writer.Byte(answer.error);
      writer.Bool(answer.value);
      break;
    case CacheStorageAnswerOp::kNames:
      writer.U64(answer.question);
      writer.U32(static_cast<uint32_t>(answer.names.size()));
      for (const std::u16string& name : answer.names) {
        writer.Sized16(name);
      }
      break;
    case CacheStorageAnswerOp::kResponses:
      writer.U64(answer.question);
      writer.Byte(answer.error);
      writer.U32(static_cast<uint32_t>(answer.responses.size()));
      for (const PageCacheResponse& response : answer.responses) {
        WriteResponse(writer, response);
      }
      break;
    case CacheStorageAnswerOp::kRequests:
      writer.U64(answer.question);
      writer.Byte(answer.error);
      writer.U32(static_cast<uint32_t>(answer.requests.size()));
      for (const PageCacheRequest& request : answer.requests) {
        WriteRequest(writer, request);
      }
      break;
    case CacheStorageAnswerOp::kBatchDone:
      writer.U64(answer.question);
      writer.Byte(answer.error);
      writer.Bool(answer.message.has_value());
      if (answer.message) {
        writer.Sized(*answer.message);
      }
      break;
    case CacheStorageAnswerOp::kBodyWanted:
      writer.U64(answer.body);
      writer.U32(answer.most);
      break;
    case CacheStorageAnswerOp::kBodyData:
      writer.U64(answer.body);
      writer.Raw(answer.bytes);
      break;
    case CacheStorageAnswerOp::kBodyEnd:
      writer.U64(answer.body);
      writer.I32(answer.net_error);
      break;
    case CacheStorageAnswerOp::kEstimated:
      writer.U64(answer.question);
      writer.I64(answer.usage);
      writer.I64(answer.quota);
      writer.I64(answer.indexed_db);
      writer.I64(answer.caches);
      break;
  }
  return writer.Take();
}

bool CacheStorageAnswerFits(const PageCacheStorageAnswer& answer) {
  return EncodeCacheStorageAnswer(answer).size() <= kMaxPayloadSize;
}

base::expected<PageCacheStorageAnswer, std::string> ReadCacheStorageAnswer(
    std::string_view payload) {
  Reader reader(payload);
  PageCacheStorageAnswer answer;
  uint8_t op = 0;
  if (!reader.Byte(op) || op == 0 ||
      op > static_cast<uint8_t>(CacheStorageAnswerOp::kMaxValue)) {
    return base::unexpected("a Cache API answer of an unknown operation");
  }
  answer.op = static_cast<CacheStorageAnswerOp>(op);
  bool valid = false;
  switch (answer.op) {
    case CacheStorageAnswerOp::kOpened:
      valid = reader.U64(answer.question) && reader.Byte(answer.error) &&
              reader.U64(answer.cache);
      break;
    case CacheStorageAnswerOp::kBoolean:
      valid = reader.U64(answer.question) && reader.Byte(answer.error) &&
              reader.Bool(answer.value);
      break;
    case CacheStorageAnswerOp::kNames: {
      uint32_t count = 0;
      valid = reader.U64(answer.question) && reader.Count(count, 4);
      for (uint32_t i = 0; valid && i < count; ++i) {
        valid = reader.Sized16(answer.names.emplace_back());
      }
      break;
    }
    case CacheStorageAnswerOp::kResponses: {
      uint32_t count = 0;
      valid = reader.U64(answer.question) && reader.Byte(answer.error) &&
              reader.Count(count, 4);
      for (uint32_t i = 0; valid && i < count; ++i) {
        valid = ReadResponse(reader, answer.responses.emplace_back());
      }
      break;
    }
    case CacheStorageAnswerOp::kRequests: {
      uint32_t count = 0;
      valid = reader.U64(answer.question) && reader.Byte(answer.error) &&
              reader.Count(count, 12);
      for (uint32_t i = 0; valid && i < count; ++i) {
        valid = ReadRequest(reader, answer.requests.emplace_back());
      }
      break;
    }
    case CacheStorageAnswerOp::kBatchDone: {
      bool has_message = false;
      valid = reader.U64(answer.question) && reader.Byte(answer.error) &&
              reader.Bool(has_message);
      if (valid && has_message) {
        answer.message.emplace();
        valid = reader.Sized(*answer.message) &&
                base::IsStringUTF8(*answer.message);
      }
      break;
    }
    case CacheStorageAnswerOp::kBodyWanted:
      valid = reader.U64(answer.body) && reader.U32(answer.most) &&
              answer.most > 0;
      break;
    case CacheStorageAnswerOp::kBodyData:
      valid = reader.U64(answer.body);
      answer.bytes.assign(reader.Rest());
      valid = valid && !answer.bytes.empty() &&
              answer.bytes.size() <= kMaxBodyPartSize;
      break;
    case CacheStorageAnswerOp::kBodyEnd:
      valid = reader.U64(answer.body) && reader.I32(answer.net_error);
      break;
    case CacheStorageAnswerOp::kEstimated:
      valid = reader.U64(answer.question) && reader.I64(answer.usage) &&
              reader.I64(answer.quota) && reader.I64(answer.indexed_db) &&
              reader.I64(answer.caches) && answer.usage >= 0 &&
              answer.quota >= 0 && answer.indexed_db >= 0 &&
              answer.caches >= 0;
      break;
  }
  if (!valid || !reader.AtEnd() || answer.error > kMaxCacheStorageError) {
    return base::unexpected("a Cache API answer that is not valid");
  }
  return answer;
}

}  // namespace mitto
