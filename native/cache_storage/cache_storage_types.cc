#include "mitto/cache_storage/cache_storage_types.h"

#include <algorithm>

#include "base/containers/span.h"
#include "base/pickle.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace mitto {
namespace {

// Changes with any change to what SerializeCacheMetadata() writes.
constexpr int kMetadataVersion = 1;

void WriteHeaders(base::Pickle& pickle, const CacheHeaderMap& headers) {
  pickle.WriteUInt64(headers.size());
  for (const auto& [name, value] : headers) {
    pickle.WriteString(name);
    pickle.WriteString(value);
  }
}

bool ReadHeaders(base::PickleIterator& iterator, CacheHeaderMap& headers) {
  uint64_t count = 0;
  if (!iterator.ReadUInt64(&count)) {
    return false;
  }
  std::vector<std::pair<std::string, std::string>> lines;
  for (uint64_t i = 0; i < count; ++i) {
    std::string name;
    std::string value;
    if (!iterator.ReadString(&name) || !iterator.ReadString(&value)) {
      return false;
    }
    lines.emplace_back(std::move(name), std::move(value));
  }
  headers = CacheHeaderMap(std::move(lines));
  return true;
}

void WriteStrings(base::Pickle& pickle, const std::vector<std::string>& list) {
  pickle.WriteUInt64(list.size());
  for (const std::string& item : list) {
    pickle.WriteString(item);
  }
}

bool ReadStrings(base::PickleIterator& iterator,
                 std::vector<std::string>& list) {
  uint64_t count = 0;
  if (!iterator.ReadUInt64(&count)) {
    return false;
  }
  for (uint64_t i = 0; i < count; ++i) {
    std::string item;
    if (!iterator.ReadString(&item)) {
      return false;
    }
    list.push_back(std::move(item));
  }
  return true;
}

void WriteOptional(base::Pickle& pickle,
                   const std::optional<std::string>& value) {
  pickle.WriteBool(value.has_value());
  if (value) {
    pickle.WriteString(*value);
  }
}

bool ReadOptional(base::PickleIterator& iterator,
                  std::optional<std::string>& value) {
  bool has_value = false;
  if (!iterator.ReadBool(&has_value)) {
    return false;
  }
  if (!has_value) {
    value.reset();
    return true;
  }
  std::string read;
  if (!iterator.ReadString(&read)) {
    return false;
  }
  value = std::move(read);
  return true;
}

}  // namespace

CacheRequest::CacheRequest() = default;
CacheRequest::CacheRequest(const CacheRequest&) = default;
CacheRequest::CacheRequest(CacheRequest&&) = default;
CacheRequest& CacheRequest::operator=(const CacheRequest&) = default;
CacheRequest& CacheRequest::operator=(CacheRequest&&) = default;
CacheRequest::~CacheRequest() = default;

CacheResponse::CacheResponse() = default;
CacheResponse::CacheResponse(const CacheResponse&) = default;
CacheResponse::CacheResponse(CacheResponse&&) = default;
CacheResponse& CacheResponse::operator=(const CacheResponse&) = default;
CacheResponse& CacheResponse::operator=(CacheResponse&&) = default;
CacheResponse::~CacheResponse() = default;

CacheMetadata::CacheMetadata() = default;
CacheMetadata::CacheMetadata(CacheMetadata&&) = default;
CacheMetadata& CacheMetadata::operator=(CacheMetadata&&) = default;
CacheMetadata::~CacheMetadata() = default;

std::string SerializeCacheMetadata(const CacheMetadata& metadata) {
  base::Pickle pickle;
  pickle.WriteInt(kMetadataVersion);
  pickle.WriteInt64(metadata.entry_time.ToInternalValue());
  // CacheRequest.
  pickle.WriteString(metadata.request.method);
  WriteHeaders(pickle, metadata.request.headers);
  WriteOptional(pickle, metadata.fragment);
  pickle.WriteBool(metadata.request.is_reload);
  pickle.WriteBool(metadata.request.is_history_navigation);
  // CacheResponse.
  const CacheResponse& response = metadata.response;
  pickle.WriteInt(response.status_code);
  pickle.WriteString(response.status_text);
  pickle.WriteInt(static_cast<int>(response.response_type));
  WriteHeaders(pickle, response.headers);
  pickle.WriteUInt64(response.url_list.size());
  for (const GURL& url : response.url_list) {
    pickle.WriteString(url.spec());
  }
  pickle.WriteInt64(response.response_time.ToInternalValue());
  WriteStrings(pickle, response.cors_exposed_header_names);
  pickle.WriteInt(response.connection_info);
  pickle.WriteString(response.alpn_negotiated_protocol);
  pickle.WriteBool(response.was_fetched_via_spdy);
  WriteOptional(pickle, response.mime_type);
  WriteOptional(pickle, response.request_method);
  pickle.WriteInt64(response.padding);
  pickle.WriteInt64(response.side_data_padding);
  pickle.WriteBool(response.request_include_credentials);
  pickle.WriteBool(response.timing_allow_passed);
  pickle.WriteBool(response.has_range_requested);
  return std::string(pickle.AsStringView());
}

std::optional<CacheMetadata> ParseCacheMetadata(const std::string& bytes) {
  base::PickleIterator iterator =
      base::PickleIterator::WithData(base::as_byte_span(bytes));
  int version = 0;
  if (!iterator.ReadInt(&version) || version != kMetadataVersion) {
    return std::nullopt;
  }
  CacheMetadata metadata;
  int64_t entry_time = 0;
  if (!iterator.ReadInt64(&entry_time) ||
      !iterator.ReadString(&metadata.request.method) ||
      !ReadHeaders(iterator, metadata.request.headers) ||
      !ReadOptional(iterator, metadata.fragment) ||
      !iterator.ReadBool(&metadata.request.is_reload) ||
      !iterator.ReadBool(&metadata.request.is_history_navigation)) {
    return std::nullopt;
  }
  metadata.entry_time = base::Time::FromInternalValue(entry_time);
  CacheResponse& response = metadata.response;
  int response_type = 0;
  uint64_t url_count = 0;
  if (!iterator.ReadInt(&response.status_code) ||
      !iterator.ReadString(&response.status_text) ||
      !iterator.ReadInt(&response_type) || response_type < 0 ||
      response_type > static_cast<int>(CacheResponseType::kMaxValue) ||
      !ReadHeaders(iterator, response.headers) ||
      !iterator.ReadUInt64(&url_count)) {
    return std::nullopt;
  }
  response.response_type = static_cast<CacheResponseType>(response_type);
  for (uint64_t i = 0; i < url_count; ++i) {
    std::string url;
    if (!iterator.ReadString(&url)) {
      return std::nullopt;
    }
    response.url_list.emplace_back(url);
  }
  int64_t response_time = 0;
  if (!iterator.ReadInt64(&response_time) ||
      !ReadStrings(iterator, response.cors_exposed_header_names) ||
      !iterator.ReadInt(&response.connection_info) ||
      !iterator.ReadString(&response.alpn_negotiated_protocol) ||
      !iterator.ReadBool(&response.was_fetched_via_spdy) ||
      !ReadOptional(iterator, response.mime_type) ||
      !ReadOptional(iterator, response.request_method) ||
      !iterator.ReadInt64(&response.padding) ||
      !iterator.ReadInt64(&response.side_data_padding) ||
      !iterator.ReadBool(&response.request_include_credentials) ||
      !iterator.ReadBool(&response.timing_allow_passed) ||
      !iterator.ReadBool(&response.has_range_requested)) {
    return std::nullopt;
  }
  response.response_time = base::Time::FromInternalValue(response_time);
  return metadata;
}

GURL NormalizeCacheUrl(const GURL& url) {
  if (url.has_ref()) {
    return url.GetWithoutRef();
  }
  return url;
}

bool VaryMatches(const CacheHeaderMap& request,
                 const CacheHeaderMap& cached_request,
                 CacheResponseType response_type,
                 const CacheHeaderMap& response_headers) {
  if (response_type == CacheResponseType::kOpaque) {
    return true;
  }
  const auto vary = std::ranges::find_if(response_headers, [](const auto& line) {
    return base::EqualsCaseInsensitiveASCII(line.first, "vary");
  });
  if (vary == response_headers.end()) {
    return true;
  }
  for (const std::string& trimmed :
       base::SplitString(vary->second, ",", base::TRIM_WHITESPACE,
                         base::SPLIT_WANT_NONEMPTY)) {
    if (trimmed == "*") {
      return false;
    }
    // FetchAPIRequestHeadersMap compares names without case.
    const auto find = [&trimmed](const CacheHeaderMap& map) {
      return std::ranges::find_if(map, [&trimmed](const auto& line) {
        return base::EqualsCaseInsensitiveASCII(line.first, trimmed);
      });
    };
    const auto request_line = find(request);
    const auto cached_line = find(cached_request);
    if ((request_line == request.end()) !=
        (cached_line == cached_request.end())) {
      return false;
    }
    if (request_line != request.end() &&
        request_line->second != cached_line->second) {
      return false;
    }
  }
  return true;
}

bool ShouldPadResponseType(CacheResponseType type) {
  return type == CacheResponseType::kOpaque ||
         type == CacheResponseType::kOpaqueRedirect;
}

base::CheckedNumeric<uint64_t> RequiredSpaceForRequest(
    const CacheRequest& request) {
  base::CheckedNumeric<uint64_t> space = request.method.size();
  space += request.url.spec().size();
  for (const auto& [name, value] : request.headers) {
    space += name.size();
    space += value.size();
  }
  return space;
}

base::CheckedNumeric<uint64_t> RequiredSpaceForResponse(
    const CacheResponse& response) {
  base::CheckedNumeric<uint64_t> space = response.body_size;
  space += base::CheckedNumeric<uint64_t>(response.padding);
  space += response.status_text.size();
  for (const auto& [name, value] : response.headers) {
    space += name.size();
    space += value.size();
  }
  for (const std::string& name : response.cors_exposed_header_names) {
    space += name.size();
  }
  for (const GURL& url : response.url_list) {
    space += url.spec().size();
  }
  return space;
}

size_t EstimatedRequestSize(const CacheRequest& request) {
  size_t size = sizeof(CacheRequest) + request.url.spec().size();
  for (const auto& [name, value] : request.headers) {
    size += name.size() + value.size();
  }
  return size;
}

size_t EstimatedResponseSize(const CacheResponse& response,
                             size_t cache_name_size) {
  size_t size = sizeof(CacheResponse);
  for (const GURL& url : response.url_list) {
    size += url.spec().size();
  }
  size += response.status_text.size();
  // The response names its cache, as cache_storage_cache_name does.
  size += cache_name_size;
  for (const auto& [name, value] : response.headers) {
    size += name.size() + value.size();
  }
  for (const std::string& name : response.cors_exposed_header_names) {
    size += name.size();
  }
  return size;
}

}  // namespace mitto
