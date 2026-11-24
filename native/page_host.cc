#include "mitto/page_host.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "mitto/cross_origin_resource_policy.h"
#include "mitto/mime_types.h"
#include "mitto/mitto.h"
#include "mitto/page_cache_storage.h"
#include "mitto/page_indexed_db.h"
#include "mitto/preconnect_manager.h"
#include "mitto/ready_requests.h"
#include "mitto/sec_header_helpers.h"
#include "mitto/storage/storage_area_impl.h"
#include "mitto/storage/storage_context.h"
#include "mitto/storage_quota.h"
#include "net/base/io_buffer.h"
#include "net/base/isolation_info.h"
#include "net/base/mime_sniffer.h"
#include "net/base/net_errors.h"
#include "net/base/schemeful_site.h"
#include "net/base/url_util.h"
#include "net/cookies/cookie_access_delegate.h"
#include "net/cookies/cookie_access_params.h"
#include "net/cookies/cookie_change_dispatcher.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_inclusion_status.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "net/cookies/cookie_store.h"
#include "net/cookies/cookie_util.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_content_disposition.h"
#include "net/http/http_util.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

// Whether `key` and `value`, if any, are as Blink's CachedStorageArea writes
// them: for localStorage a byte that says Latin-1 or UTF-16 and then its code
// units, and for sessionStorage a key in UTF-8 and a value in UTF-16. The
// Session's store counts on a localStorage key never being empty.
bool IsStorageAsBlinkWrites(PageStorageKind kind,
                            std::string_view key,
                            std::optional<std::string_view> value) {
  if (kind == PageStorageKind::kSession) {
    return base::IsStringUTF8AllowingNoncharacters(key) &&
           (!value || value->size() % 2 == 0);
  }
  const auto local = [](std::string_view bytes) {
    // 1 for Latin-1, and 0 for UTF-16, two bytes for each code unit.
    return !bytes.empty() &&
           (bytes[0] == 1 || (bytes[0] == 0 && bytes.size() % 2 == 1));
  };
  return local(key) && (!value || local(*value));
}

// Blink's CanChangeToUrlForHistoryApi() for a document at `document_url` of
// `document_origin`: the same scheme, user, password, host and port, any
// path, query and fragment for HTTP and HTTPS, and for a document whose origin
// is opaque only another query or fragment.
bool CanChangeToUrlForHistoryApi(const GURL& url,
                                 const url::Origin& document_origin,
                                 const GURL& document_url) {
  if (!url.is_valid()) {
    return false;
  }
  if (url.scheme() != document_url.scheme() ||
      url.username() != document_url.username() ||
      url.password() != document_url.password() ||
      url.host() != document_url.host() || url.port() != document_url.port()) {
    return false;
  }
  if (url.SchemeIsHTTPOrHTTPS()) {
    return true;
  }
  const bool differ_in_path = url.path() != document_url.path();
  const bool differ_in_query = url.query() != document_url.query();
  if (url.SchemeIsFile() && differ_in_path) {
    return false;
  }
  if (document_origin.opaque()) {
    return !differ_in_path;
  }
  if (url.IsStandard()) {
    return true;
  }
  return !differ_in_path && !differ_in_query;
}

// How many documents' navigations the library keeps for traversals: past
// this, the oldest go. The session history holds 50 entries, and the Page
// releases what none holds.
constexpr size_t kMaxNavigationRecords = 100;
// How many URLs that navigations within a document gave it the library keeps
// for traversals to its entries, and how many bytes of them: past these, the
// least recent go, and a traversal to one of them goes nowhere.
constexpr size_t kMaxSameDocumentUrls = 200;
constexpr size_t kMaxSameDocumentUrlBytes = 4 * 1024 * 1024;

constexpr std::string_view kVersionKey = "version";
// What net::HttpConnectionInfoToString() and net::NextProtoToString() call a
// protocol that is not known.
constexpr char kUnknownProtocol[] = "unknown";
constexpr std::string_view kStartKey = "start";
constexpr std::string_view kUrlKey = "url";
constexpr std::string_view kRequestsKey = "requests";
// The names of the Request Kinds in "requests", as RequestKind's values are in
// Python, and what follows the name of a kind's template with a body.
constexpr auto kRequestKindNames =
    std::to_array<std::pair<std::string_view, PageRequestKind>>({
        {"navigation", PageRequestKind::kNavigation},
        {"script", PageRequestKind::kScript},
        {"fetch", PageRequestKind::kFetch},
        {"image", PageRequestKind::kImage},
        {"style", PageRequestKind::kStyle},
        {"font", PageRequestKind::kFont},
        {"module", PageRequestKind::kModule},
        {"xhr", PageRequestKind::kXhr},
        {"worker", PageRequestKind::kWorker},
        {"beacon", PageRequestKind::kBeacon},
        {"iframe", PageRequestKind::kIframe},
        {"favicon", PageRequestKind::kFavicon},
        {"module_json", PageRequestKind::kModuleJson},
        {"prefetch", PageRequestKind::kPrefetch},
    });
constexpr std::string_view kWithBodySuffix = "_with_body";
constexpr std::string_view kSecFetchMode = "Sec-Fetch-Mode";
constexpr std::string_view kHeadersKey = "headers";
constexpr std::string_view kOptionsKey = "options";

// The levels of the messages and of the C API are the same numbers.
static_assert(static_cast<int>(ConsoleLevel::kDebug) ==
              MITTO_PAGE_CONSOLE_DEBUG);
static_assert(static_cast<int>(ConsoleLevel::kLog) == MITTO_PAGE_CONSOLE_LOG);
static_assert(static_cast<int>(ConsoleLevel::kInfo) == MITTO_PAGE_CONSOLE_INFO);
static_assert(static_cast<int>(ConsoleLevel::kWarning) ==
              MITTO_PAGE_CONSOLE_WARNING);
static_assert(static_cast<int>(ConsoleLevel::kError) ==
              MITTO_PAGE_CONSOLE_ERROR);

bool IsReport(const PageEvent& event) {
  return event.type == MITTO_PAGE_EVENT_CONSOLE ||
         event.type == MITTO_PAGE_EVENT_UNCAUGHT_ERROR ||
         event.type == MITTO_PAGE_EVENT_KNOWN_GAP;
}

// Returns the bytes that `text` names, one for each character, as the C API's
// headers are Latin-1. Returns nullopt for a character beyond U+00FF.
std::optional<std::string> ToLatin1(const std::string& text) {
  std::u16string characters;
  if (!base::UTF8ToUTF16(text.data(), text.size(), &characters)) {
    return std::nullopt;
  }
  std::string bytes;
  bytes.reserve(characters.size());
  for (const char16_t character : characters) {
    if (character > 0xFF) {
      return std::nullopt;
    }
    bytes.push_back(static_cast<char>(character));
  }
  return bytes;
}

// Reads the header lines of a template: a list of [name, value] pairs, which
// can be sent as mitto_request_start() sends its headers.
std::optional<net::HttpRequestHeaders> ReadTemplateHeaders(
    const base::Value& value) {
  if (!value.is_list()) {
    return std::nullopt;
  }
  net::HttpRequestHeaders headers;
  for (const base::Value& line : value.GetList()) {
    if (!line.is_list() || line.GetList().size() != 2 ||
        !line.GetList()[0].is_string() || !line.GetList()[1].is_string()) {
      return std::nullopt;
    }
    std::optional<std::string> name = ToLatin1(line.GetList()[0].GetString());
    std::optional<std::string> header = ToLatin1(line.GetList()[1].GetString());
    // As mitto_request_start() checks its headers.
    if (!name || !header || !net::HttpUtil::IsValidHeaderName(*name) ||
        !net::HttpUtil::IsValidHeaderValue(*header) ||
        headers.HasHeader(*name) ||
        base::EqualsCaseInsensitiveASCII(*name,
                                         net::HttpRequestHeaders::kReferer)) {
      return std::nullopt;
    }
    headers.SetHeader(*name, *header);
  }
  return headers;
}

// Reads what a request of one Request Kind sends. Its options are what
// mitto_request_start() reads, for a request made for a page, with nothing
// that the library decides for each request of a Page's: no Initiator, no
// caller's headers, body, Referer or timers.
std::optional<PageRequestTemplate> ReadTemplate(const base::Value& value) {
  if (!value.is_dict() || value.GetDict().size() != 2) {
    return std::nullopt;
  }
  const base::Value* const headers = value.GetDict().Find(kHeadersKey);
  const base::DictValue* const options = value.GetDict().FindDict(kOptionsKey);
  if (!headers || !options) {
    return std::nullopt;
  }
  PageRequestTemplate request_template;
  std::optional<net::HttpRequestHeaders> lines = ReadTemplateHeaders(*headers);
  std::optional<RequestOptions> parsed =
      ParseRequestOptions(*base::WriteJson(*options));
  if (!lines || !parsed || !parsed->fetch || parsed->fetch->initiator ||
      !parsed->fetch->fixed_headers.empty() || parsed->stream_body ||
      !parsed->referrer.is_empty() || parsed->connect_timeout ||
      parsed->read_timeout || parsed->total_timeout) {
    return std::nullopt;
  }
  request_template.headers = *std::move(lines);
  request_template.options = *std::move(parsed);
  return request_template;
}

bool ReadTemplates(const base::Value& value, PageOptions& options) {
  if (!value.is_dict()) {
    return false;
  }
  for (const auto [key, item] : value.GetDict()) {
    std::string_view name = key;
    const bool with_body = name.ends_with(kWithBodySuffix);
    if (with_body) {
      name.remove_suffix(kWithBodySuffix.size());
    }
    const auto found =
        std::ranges::find(kRequestKindNames, name,
                          &std::pair<std::string_view, PageRequestKind>::first);
    std::optional<PageRequestTemplate> request_template = ReadTemplate(item);
    if (found == kRequestKindNames.end() || !request_template) {
      return false;
    }
    (with_body ? options.requests_with_body : options.requests)
        .insert_or_assign(found->second, *std::move(request_template));
  }
  return true;
}

// The values of RequestOptions for what a Page's request names.
RequestMode ToRequestMode(PageRequestMode mode) {
  switch (mode) {
    case PageRequestMode::kKindDefault:
    case PageRequestMode::kNavigate:
      return RequestMode::kNavigate;
    case PageRequestMode::kSameOrigin:
      return RequestMode::kSameOrigin;
    case PageRequestMode::kNoCors:
      return RequestMode::kNoCors;
    case PageRequestMode::kCors:
      return RequestMode::kCors;
  }
}

CredentialsMode ToCredentialsMode(PageCredentialsMode credentials) {
  switch (credentials) {
    case PageCredentialsMode::kOmit:
      return CredentialsMode::kOmit;
    case PageCredentialsMode::kSameOrigin:
      return CredentialsMode::kSameOrigin;
    case PageCredentialsMode::kKindDefault:
    case PageCredentialsMode::kInclude:
      return CredentialsMode::kInclude;
  }
}

// Blink's mapping of a referrer policy to //net's, in
// network::ReferrerPolicyForUrlRequest().
// Upstream: services/network/public/cpp/request_destination.cc
// Upstream: net/url_request/referrer_policy.h
net::ReferrerPolicy ToNetReferrerPolicy(PageReferrerPolicy policy) {
  switch (policy) {
    case PageReferrerPolicy::kNoReferrer:
      return net::ReferrerPolicy::NO_REFERRER;
    case PageReferrerPolicy::kNoReferrerWhenDowngrade:
      return net::ReferrerPolicy::CLEAR_ON_TRANSITION_FROM_SECURE_TO_INSECURE;
    case PageReferrerPolicy::kOrigin:
      return net::ReferrerPolicy::ORIGIN;
    case PageReferrerPolicy::kOriginWhenCrossOrigin:
      return net::ReferrerPolicy::ORIGIN_ONLY_ON_TRANSITION_CROSS_ORIGIN;
    case PageReferrerPolicy::kSameOrigin:
      return net::ReferrerPolicy::CLEAR_ON_TRANSITION_CROSS_ORIGIN;
    case PageReferrerPolicy::kStrictOrigin:
      return net::ReferrerPolicy::
          ORIGIN_CLEAR_ON_TRANSITION_FROM_SECURE_TO_INSECURE;
    case PageReferrerPolicy::kUnsafeUrl:
      return net::ReferrerPolicy::NEVER_CLEAR;
    case PageReferrerPolicy::kDocument:
    case PageReferrerPolicy::kStrictOriginWhenCrossOrigin:
      return net::ReferrerPolicy::REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN;
  }
}

// Puts what a Page's request names in place of what its kind sends: its mode,
// in the place of the kind's Sec-Fetch-Mode line, with an Origin line first
// for an element in cors mode, where Blink puts it, its credentials mode, its
// referrer policy and the URL that Referer starts from, and its priority, as
// Python's build_request() does for a caller's credentials and mode.
void ApplyPageRequest(const PageRequest& asked,
                      net::HttpRequestHeaders& headers,
                      RequestOptions& options) {
  FetchParams& fetch = *options.fetch;
  if (asked.mode != PageRequestMode::kKindDefault &&
      ToRequestMode(asked.mode) != fetch.mode) {
    fetch.mode = ToRequestMode(asked.mode);
    if (headers.HasHeader(kSecFetchMode)) {
      headers.SetHeader(kSecFetchMode, RequestModeToString(fetch.mode));
    }
    if (fetch.mode == RequestMode::kCors && fetch.destination != "empty" &&
        !headers.HasHeader(net::HttpRequestHeaders::kOrigin)) {
      net::HttpRequestHeaders with_origin;
      with_origin.SetHeader(net::HttpRequestHeaders::kOrigin, "null");
      with_origin.MergeFrom(headers);
      headers = std::move(with_origin);
    }
  }
  if (asked.credentials != PageCredentialsMode::kKindDefault) {
    fetch.credentials_mode = ToCredentialsMode(asked.credentials);
  }
  if (asked.no_referrer) {
    fetch.referrer_policy = net::ReferrerPolicy::NO_REFERRER;
  } else if (asked.referrer_policy != PageReferrerPolicy::kDocument) {
    fetch.referrer_policy = ToNetReferrerPolicy(asked.referrer_policy);
  }
  if (asked.priority != 0) {
    options.priority = static_cast<net::RequestPriority>(asked.priority - 1);
  } else if (asked.synchronous) {
    // ResourceFetcher::ComputeLoadPriority(): a synchronous request is very
    // high.
    options.priority = net::HIGHEST;
  }
  if (asked.incremental != 0) {
    options.priority_incremental = asked.incremental == 2;
  }
}

// Moves Origin to just after User-Agent where it comes before it, as a
// worker's request has them: Chrome 157's worker sends a module script's
// User-Agent, then Origin, where a window's names Origin first.
void PutOriginAfterUserAgent(net::HttpRequestHeaders& headers) {
  const net::HttpRequestHeaders::HeaderVector& lines =
      headers.GetHeaderVector();
  const auto named = [&lines](std::string_view name) {
    return std::ranges::find_if(lines, [name](const auto& line) {
      return base::EqualsCaseInsensitiveASCII(line.key, name);
    });
  };
  const auto origin = named(net::HttpRequestHeaders::kOrigin);
  const auto user_agent = named(net::HttpRequestHeaders::kUserAgent);
  if (origin == lines.end() || user_agent == lines.end() ||
      origin > user_agent) {
    return;
  }
  net::HttpRequestHeaders moved;
  for (auto line = lines.begin(); line != lines.end(); ++line) {
    if (line == origin) {
      continue;
    }
    moved.SetHeader(line->key, line->value);
    if (line == user_agent) {
      moved.SetHeader(origin->key, origin->value);
    }
  }
  headers = std::move(moved);
}

// Returns the header lines of a response that a Page may see: all but those
// that set cookies, so that no HttpOnly cookie reaches a Page's process.
// Blink never shows them to a page's scripts either.
// Upstream: third_party/blink/renderer/platform/loader/fetch/fetch_utils.cc
std::string HeadersForPage(std::string_view lines) {
  std::string kept;
  for (std::string_view line : base::SplitStringPieceUsingSubstr(
           lines, "\r\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const std::string_view name = line.substr(0, line.find(':'));
    if (base::EqualsCaseInsensitiveASCII(name, "set-cookie") ||
        base::EqualsCaseInsensitiveASCII(name, "set-cookie2")) {
      continue;
    }
    base::StrAppend(&kept, {line, "\n"});
  }
  return kept;
}

}  // namespace

PageRequestTemplate::PageRequestTemplate() = default;
PageRequestTemplate::PageRequestTemplate(const PageRequestTemplate&) = default;
PageRequestTemplate& PageRequestTemplate::operator=(
    const PageRequestTemplate&) = default;
PageRequestTemplate::~PageRequestTemplate() = default;

PageOptions::PageOptions() = default;
PageOptions::PageOptions(PageOptions&&) = default;
PageOptions& PageOptions::operator=(PageOptions&&) = default;
PageOptions::~PageOptions() = default;

std::optional<PageOptions> ParsePageOptions(std::string_view json) {
  const std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict || dict->FindInt(kVersionKey) != MITTO_PAGE_OPTIONS_VERSION) {
    return std::nullopt;
  }
  PageOptions options;
  bool has_start = false;
  for (const auto [key, value] : *dict) {
    if (key == kVersionKey) {
      continue;
    }
    if (key == kUrlKey) {
      if (!value.is_string()) {
        return std::nullopt;
      }
      // One that is not valid fails as a request for it fails, with
      // ERR_INVALID_URL.
      options.url = GURL(value.GetString());
      continue;
    }
    if (key == kRequestsKey) {
      if (!ReadTemplates(value, options)) {
        return std::nullopt;
      }
      continue;
    }
    if (key != kStartKey || !value.is_dict()) {
      return std::nullopt;
    }
    // The Page checks the time zone and the locale, as only it holds ICU's
    // whole data.
    base::expected<PageStart, std::string> start = ReadStart(value.GetDict());
    if (!start.has_value()) {
      return std::nullopt;
    }
    options.start = *std::move(start);
    has_start = true;
  }
  // A navigation sends what the Persona's says.
  if (!has_start || (options.url && !options.requests.contains(
                                        PageRequestKind::kNavigation))) {
    return std::nullopt;
  }
  return options;
}

PageEvent::PageEvent() = default;
PageEvent::PageEvent(PageEvent&&) = default;
PageEvent& PageEvent::operator=(PageEvent&&) = default;
PageEvent::~PageEvent() = default;

PageEvents::PageEvents() = default;
PageEvents::~PageEvents() = default;

uint64_t PageEvents::NewCall() {
  base::AutoLock lock(lock_);
  return next_call_++;
}

int PageEvents::NextEvent(std::optional<base::TimeDelta> timeout,
                          PageEvent& event) {
  base::AutoLock lock(lock_);
  if (timeout) {
    const base::TimeTicks deadline = base::TimeTicks::Now() + *timeout;
    while (events_.empty() && !waits_ended_) {
      const base::TimeDelta left = deadline - base::TimeTicks::Now();
      if (!left.is_positive()) {
        return net::ERR_IO_PENDING;
      }
      event_waiting_.TimedWait(left);
    }
  } else {
    while (events_.empty() && !waits_ended_) {
      event_waiting_.Wait();
    }
  }
  if (events_.empty()) {
    return net::ERR_IO_PENDING;
  }
  event = std::move(events_.front());
  events_.pop_front();
  if (IsReport(event)) {
    --waiting_reports_;
    waiting_report_bytes_ -= event.message.size();
  }
  return net::OK;
}

void PageEvents::EndWaits() {
  base::AutoLock lock(lock_);
  waits_ended_ = true;
  event_waiting_.Broadcast();
}

void PageEvents::Push(PageEvent event) {
  base::AutoLock lock(lock_);
  events_.push_back(std::move(event));
  event_waiting_.Signal();
}

int PageEvents::PushReport(PageEvent event) {
  base::AutoLock lock(lock_);
  std::optional<PageEvent> note;
  if (dropped_reports_ > 0) {
    note.emplace();
    note->type = MITTO_PAGE_EVENT_CONSOLE;
    note->level = MITTO_PAGE_CONSOLE_WARNING;
    note->message = base::StrCat(
        {"the library dropped ", base::NumberToString(dropped_reports_),
         " console messages and uncaught errors of this Page, because too "
         "many waited untaken"});
  }
  const size_t pushed = note ? 2 : 1;
  const size_t bytes = event.message.size() + (note ? note->message.size() : 0);
  // Neither sum can wrap: what waits stays within the limits, and one
  // message is at most a frame.
  if (waiting_reports_ + pushed > kMaxWaitingReports ||
      waiting_report_bytes_ + bytes > kMaxWaitingReportBytes) {
    ++dropped_reports_;
    return 0;
  }
  if (note) {
    AddReport(*std::move(note));
    dropped_reports_ = 0;
  }
  AddReport(std::move(event));
  event_waiting_.Signal();
  return static_cast<int>(pushed);
}

void PageEvents::AddReport(PageEvent event) {
  ++waiting_reports_;
  waiting_report_bytes_ += event.message.size();
  events_.push_back(std::move(event));
}

void PageEvents::PushEnded(int32_t error, std::string message) {
  base::AutoLock lock(lock_);
  ended_error_ = error;
  ended_message_ = message;
  PageEvent event;
  event.type = MITTO_PAGE_EVENT_ENDED;
  event.error = error;
  event.message = std::move(message);
  events_.push_back(std::move(event));
  event_waiting_.Signal();
}

void PageEvents::PushCallAfterEnd(uint64_t call) {
  base::AutoLock lock(lock_);
  PageEvent event;
  event.type = MITTO_PAGE_EVENT_EVALUATED;
  event.error = ended_error_;
  event.call = call;
  event.message = ended_message_;
  events_.push_back(std::move(event));
  event_waiting_.Signal();
}

namespace {

// A time of base::TimeTicks as the messages carry it, 0 for a null one.
int64_t Microseconds(base::TimeTicks time) {
  return time.is_null() ? 0 : (time - base::TimeTicks()).InMicroseconds();
}

PageLoadTiming PageLoadTimingOf(const net::LoadTimingInfo& info) {
  PageLoadTiming timing;
  timing.request_start = Microseconds(info.request_start);
  timing.domain_lookup_start =
      Microseconds(info.connect_timing.domain_lookup_start);
  timing.domain_lookup_end =
      Microseconds(info.connect_timing.domain_lookup_end);
  timing.connect_start = Microseconds(info.connect_timing.connect_start);
  timing.connect_end = Microseconds(info.connect_timing.connect_end);
  timing.ssl_start = Microseconds(info.connect_timing.ssl_start);
  timing.send_start = Microseconds(info.send_start);
  timing.receive_headers_start = Microseconds(info.receive_headers_start);
  timing.receive_headers_end = Microseconds(info.receive_headers_end);
  timing.receive_non_informational_headers_start =
      Microseconds(info.receive_non_informational_headers_start);
  timing.socket_reused = info.socket_reused;
  return timing;
}

// The values of a response's `name` lines, joined as
// net::HttpResponseHeaders::GetNormalizedHeader() joins them, or nullopt for
// none.
std::optional<std::string> HeaderValue(std::string_view lines,
                                       std::string_view name) {
  std::optional<std::string> value;
  for (const std::string_view line : base::SplitStringPieceUsingSubstr(
           lines, "\r\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos ||
        !base::EqualsCaseInsensitiveASCII(line.substr(0, colon), name)) {
      continue;
    }
    const std::string_view item =
        base::TrimWhitespaceASCII(line.substr(colon + 1), base::TRIM_ALL);
    value = value ? base::StrCat({*value, ", ", item}) : std::string(item);
  }
  return value;
}

// The lines of a response that its cross-origin policies come from.
PolicyHeaders PolicyHeadersOf(std::string_view lines) {
  PolicyHeaders headers;
  headers.coep = HeaderValue(lines, "Cross-Origin-Embedder-Policy");
  headers.coep_report_only =
      HeaderValue(lines, "Cross-Origin-Embedder-Policy-Report-Only");
  headers.coop = HeaderValue(lines, "Cross-Origin-Opener-Policy");
  headers.coop_report_only =
      HeaderValue(lines, "Cross-Origin-Opener-Policy-Report-Only");
  return headers;
}

// The MIME type of a response, lowercased, as net::HttpUtil::ParseContentType()
// reads Content-Type, or "" for none.
std::string MimeTypeOf(const RequestEvent& event) {
  std::string mime_type;
  std::string charset;
  bool had_charset = false;
  if (const std::optional<std::string> content_type =
          HeaderValue(event.headers, "Content-Type")) {
    net::HttpUtil::ParseContentType(*content_type, &mime_type, &charset,
                                    &had_charset, nullptr);
  }
  return mime_type;
}

// NavigationRequest::OnResponseStarted() and download_utils::MustDownload():
// Chrome makes no document of a navigation's response that is a 204 or a 205,
// nor of a download, which is one that Content-Disposition says is an
// attachment, or whose MIME type Chrome does not show. One with no MIME type
// is sniffed, which the Page reads as HTML.
// Upstream: content/browser/renderer_host/navigation_request.cc
// Upstream: content/browser/loader/navigation_url_loader_impl.cc
// Upstream: content/browser/loader/download_utils_impl.cc
bool MakesDocument(const RequestEvent& event) {
  if (event.status == 204 || event.status == 205) {
    return false;
  }
  if (const std::optional<std::string> disposition =
          HeaderValue(event.headers, "Content-Disposition");
      disposition && net::HttpContentDisposition(*disposition, std::string())
                         .is_attachment()) {
    return false;
  }
  const std::string mime_type = MimeTypeOf(event);
  return mime_type.empty() || IsSupportedMimeType(mime_type);
}

// URLLoader's ShouldSniffContent() for a navigation, which asks for
// kURLLoadOptionSniffMimeType: Chrome's network service holds back a
// response whose MIME type net::ShouldSniffMimeType() would sniff, such as
// text/plain or none, unless the first value of X-Content-Type-Options is
// nosniff.
// Upstream: services/network/url_loader.cc
// Upstream: net/base/mime_sniffer.cc
bool ShouldSniffContent(const RequestEvent& event,
                        const std::string& mime_type) {
  if (const std::optional<std::string> options =
          HeaderValue(event.headers, "X-Content-Type-Options")) {
    const std::string_view first = base::TrimWhitespaceASCII(
        std::string_view(*options).substr(0, options->find(',')),
        base::TRIM_ALL);
    if (base::EqualsCaseInsensitiveASCII(first, "nosniff")) {
      return false;
    }
  }
  return net::ShouldSniffMimeType(GURL(event.url),
                                  /*http_response_headers=*/nullptr, mime_type);
}

// network::ParseTimingAllowOrigin() and TimingAllowOriginCheck(): whether a
// Timing-Allow-Origin value lets `origin` see the timing.
// Upstream: services/network/public/cpp/timing_allow_origin_parser.cc
bool TimingAllowOriginAllows(const std::string& value,
                             const url::Origin& origin) {
  if (value == "*") {
    return true;
  }
  const std::string serialized = origin.Serialize();
  net::HttpUtil::ValuesIterator values(value, /*delimiter=*/',');
  while (values.GetNext()) {
    if (values.value() == "*" || values.value() == serialized) {
      return true;
    }
  }
  return false;
}

}  // namespace

KeepaliveInFlight::KeepaliveInFlight() = default;
KeepaliveInFlight::~KeepaliveInFlight() = default;

namespace {

// What a keepalive request counts for among the bytes in flight: its URL and
// its header lines, as URLLoaderFactory counts them.
// Upstream: services/network/url_loader_factory.cc
size_t KeepaliveBytes(const GURL& url, const net::HttpRequestHeaders& headers) {
  size_t bytes = url.spec().size();
  for (const net::HttpRequestHeaders::HeaderKeyValuePair& header :
       headers.GetHeaderVector()) {
    bytes += header.key.size() + header.value.size();
  }
  return bytes;
}

// A keepalive request's place among those in flight of its Session and of its
// Page, which it gives back when it ends.
class KeepaliveShare {
 public:
  KeepaliveShare(scoped_refptr<KeepaliveInFlight> session,
                 scoped_refptr<KeepaliveInFlight> page,
                 size_t bytes)
      : session_(std::move(session)), page_(std::move(page)), bytes_(bytes) {
    ++session_->requests;
    ++page_->requests;
    page_->bytes += bytes_;
  }

  KeepaliveShare(const KeepaliveShare&) = delete;
  KeepaliveShare& operator=(const KeepaliveShare&) = delete;

  ~KeepaliveShare() {
    --session_->requests;
    --page_->requests;
    page_->bytes -= bytes_;
  }

 private:
  const scoped_refptr<KeepaliveInFlight> session_;
  const scoped_refptr<KeepaliveInFlight> page_;
  const size_t bytes_;
};

}  // namespace

// The checks that a Page's request is held to on each of its responses,
// redirects included, before the redirect is followed or any of the response
// goes to the Page, as Chrome's network service and browser hold them: the
// Fetch Standard's CORS check, mode same-origin's, Cross-Origin-Resource-
// Policy, and a worker's embedder policy. A keepalive request takes them along
// when it outlives its Page.
// Upstream: services/network/cors/cors_url_loader.cc
// Upstream: services/network/public/cpp/cors/cors.cc
// Upstream: content/browser/worker_host/dedicated_worker_host.cc
class PageHost::ResponseChecks {
 public:
  ResponseChecks(FetchChecks checks, url::Origin document_origin, GURL url)
      : checks_(checks),
        document_origin_(std::move(document_origin)),
        last_url_(std::move(url)) {}

  const FetchChecks& checks() const { return checks_; }
  // The origin of the document that asked, opaque for a navigation.
  const url::Origin& document_origin() const { return document_origin_; }
  // Whether a hop has gone to another origin than the document's, which
  // makes the response tainting cors.
  bool cors() const { return cors_; }

  // Whether the response that `event` describes passes: net::OK, or the net
  // error that ends the request.
  int Check(const RequestEvent& event) {
    // NavigationRequest::WillRedirectRequest(): a frame's navigation that a
    // redirect takes to the URL of two of the frame's ancestors' documents,
    // but for the fragment, is canceled there, as IsSelfReferentialURL() has
    // it, and the frame keeps its document.
    if (event.type == MITTO_EVENT_REDIRECT_FOLLOWED &&
        std::ranges::count_if(checks_.frame_ancestor_urls,
                              [to = GURL(event.new_url)](const GURL& url) {
                                return url.EqualsIgnoringRef(to);
                              }) >= 2) {
      return net::ERR_ABORTED;
    }
    if (!AllowsByCors(event)) {
      return net::ERR_FAILED;
    }
    // URLLoader::OnReceivedRedirect() and OnResponseStarted(): each response
    // of a request of mode no-cors is held to its
    // Cross-Origin-Resource-Policy, with the embedder policy of the context
    // that asked.
    if (checks_.no_cors &&
        IsBlockedByResourcePolicy(
            GURL(event.url), document_origin_,
            HeaderValue(event.headers, "Cross-Origin-Resource-Policy"),
            event.included_credentials, checks_.embedder_policy)) {
      return net::ERR_BLOCKED_BY_RESPONSE;
    }
    // DedicatedWorkerHost::DidStartScriptLoad(): a worker's script whose
    // embedder policy is not compatible with its creator's fails, with
    // kCoepFrameResourceNeedsCoepHeader.
    if (event.type == MITTO_EVENT_RESPONSE_STARTED &&
        checks_.creator_embedder_policy &&
        !WorkerEmbedderPolicyAllowed(
            *checks_.creator_embedder_policy,
            EmbedderPolicyOf(PolicyHeadersOf(event.headers)))) {
      return net::ERR_BLOCKED_BY_RESPONSE;
    }
    return net::OK;
  }

 private:
  // The Fetch Standard's CORS check, and mode same-origin's.
  bool AllowsByCors(const RequestEvent& event) {
    if (!checks_.cors && !checks_.same_origin) {
      return true;
    }
    Reach(GURL(event.url));
    // A redirect to another origin of a fetch of mode same-origin is not
    // followed, as in Chrome. The library asks the next origin first for one
    // that would need a CORS preflight there.
    if (event.type == MITTO_EVENT_REDIRECT_FOLLOWED && checks_.same_origin &&
        !document_origin_.IsSameOriginWith(GURL(event.new_url))) {
      return false;
    }
    if (!cors_) {
      return true;
    }
    // Access-Control-Allow-Origin has to name the request's origin, which a
    // tainted one serializes as "null", or be "*" for one without
    // credentials. One with credentials also needs
    // Access-Control-Allow-Credentials: true, in lower case.
    const std::string origin =
        tainted_ ? std::string("null") : document_origin_.Serialize();
    std::optional<std::string> allowed;
    std::optional<std::string> allow_credentials;
    for (const std::string_view line : base::SplitStringPieceUsingSubstr(
             event.headers, "\r\n", base::KEEP_WHITESPACE,
             base::SPLIT_WANT_NONEMPTY)) {
      const size_t colon = line.find(':');
      if (colon == std::string_view::npos) {
        continue;
      }
      const std::string_view name = line.substr(0, colon);
      // Access-Control-Allow-Credentials counts only for a request with
      // credentials, as the CORS check reads it only then.
      std::optional<std::string>* const slot =
          base::EqualsCaseInsensitiveASCII(name, "access-control-allow-origin")
              ? &allowed
          : checks_.credentials && base::EqualsCaseInsensitiveASCII(
                                       name, "access-control-allow-credentials")
              ? &allow_credentials
              : nullptr;
      if (!slot) {
        continue;
      }
      // More than one line is a failure, as a value that lists more than one.
      if (*slot) {
        return false;
      }
      *slot = std::string(
          base::TrimWhitespaceASCII(line.substr(colon + 1), base::TRIM_ALL));
    }
    if (checks_.credentials) {
      return allowed == origin && allow_credentials == "true";
    }
    return allowed && (*allowed == "*" || *allowed == origin);
  }

  // Follows the request to its response at `url`, a redirect's or the last,
  // and works out its response tainting and its tainted origin as the Fetch
  // Standard does.
  void Reach(const GURL& url) {
    // A redirect from a URL of another origin than the document's to one of
    // yet another origin taints the request's origin.
    if (!url::IsSameOriginWith(url, last_url_) &&
        !document_origin_.IsSameOriginWith(last_url_)) {
      tainted_ = true;
    }
    // Once any hop goes to another origin, the response is a CORS one.
    if (!document_origin_.IsSameOriginWith(url)) {
      cors_ = true;
    }
    last_url_ = url;
  }

  const FetchChecks checks_;
  const url::Origin document_origin_;
  // The URL of the hop the request is at.
  GURL last_url_;
  bool cors_ = false;
  bool tainted_ = false;
};

// One request of a Page's, which hears how its job goes and sends that to the
// Page, reading the body only as far as the Page has given credit.
class PageHost::Request final : public RequestClient {
 public:
  Request(PageHost& host,
          uint64_t number,
          url::Origin document_origin,
          GURL url,
          FetchChecks checks)
      : host_(host),
        number_(number),
        first_url_(url),
        response_checks_(checks, std::move(document_origin), std::move(url)),
        start_(base::TimeTicks::Now()),
        buffer_(base::MakeRefCounted<net::IOBufferWithSize>(kBodyWindow)) {}

  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

  ~Request() override {
    // A job that has not finished would tell a client that has gone.
    if (job_) {
      job_->Abandon();
    }
  }

  uint64_t number() const { return number_; }
  bool is_navigation() const { return checks().is_navigation; }
  bool is_top_level_navigation() const { return checks().top_level; }
  // The URL the request started at.
  const GURL& first_url() const { return first_url_; }
  // A navigation whose response the Page makes no document of, such as a
  // download, whose body the library reads to its end and drops, as Chrome's
  // download manager reads it.
  bool keeps_document() const { return keeps_document_; }
  void set_keeps_document() { keeps_document_ = true; }
  // What a navigation went as, which a traversal to its document repeats.
  const FetchParams& navigation_fetch() const { return navigation_fetch_; }
  void set_navigation_fetch(const FetchParams& fetch) {
    navigation_fetch_ = fetch;
  }

  // A navigation's response that Chrome's network service holds back while
  // it sniffs the MIME type from the start of the body, as URLLoader does.
  struct Sniffing {
    RequestEvent response;
    // The MIME type before sniffing, and the one sniffed so far.
    std::string mime_type;
    std::string sniffed;
    // What has been read of the body.
    std::string body;
  };
  bool sniffing() const { return sniffing_.has_value(); }
  // The response held back while it is sniffed, or null.
  const RequestEvent* held_response() const {
    return sniffing_ ? &sniffing_->response : nullptr;
  }
  void HoldForSniffing(RequestEvent response, std::string mime_type) {
    sniffing_.emplace();
    sniffing_->response = std::move(response);
    sniffing_->mime_type = std::move(mime_type);
  }
  // Sniffs `bytes` that the body went on with, or none at its end, as
  // URLLoader::DidRead() sniffs: until net::SniffMimeType() has decided,
  // net::kMaxBytesToSniff bytes have come or the body has ended. Returns
  // whether sniffing is over.
  bool Sniff(base::span<const uint8_t> bytes, bool ended) {
    sniffing_->body.append(base::as_string_view(bytes));
    const std::string_view data =
        std::string_view(sniffing_->body).substr(0, net::kMaxBytesToSniff);
    const bool decided = net::SniffMimeType(
        data, GURL(sniffing_->response.url), sniffing_->mime_type,
        net::ForceSniffFileUrlsForHtml::kDisabled, &sniffing_->sniffed);
    return decided || ended || sniffing_->body.size() >= net::kMaxBytesToSniff;
  }
  Sniffing TakeSniffing() {
    Sniffing sniffing = *std::move(sniffing_);
    sniffing_.reset();
    return sniffing;
  }
  // The origin of the document that asked, opaque for a navigation.
  const url::Origin& document_origin() const {
    return response_checks_.document_origin();
  }
  const FetchChecks& checks() const { return response_checks_.checks(); }
  bool cors() const { return response_checks_.cors(); }
  base::TimeTicks start() const { return start_; }
  bool started() const { return started_; }
  uint32_t credit() const { return credit_; }

  // What the Performance Timeline needs of the hops so far.
  bool timing_allow_passed() const { return !timing_allow_failed_; }
  // Whether the response's tainting is opaque: that of a request of mode
  // no-cors, such as a script's or a fetch() that asks for it, once a hop has
  // left the document's origin. A navigation's is basic.
  bool opaque() const {
    return !is_navigation() && checks().no_cors && timing_cross_origin_;
  }
  uint32_t redirect_count() const { return redirect_count_; }
  bool cross_origin_redirect() const { return cross_origin_redirect_; }
  base::TimeTicks last_redirect_end() const { return last_redirect_end_; }
  const RequestTotals& totals() const { return totals_; }

  // CorsURLLoader's TAO check of the response that `event` describes, which
  // fails for good once it fails, for a Page's document at
  // `document_origin`. A navigation has no initiator, and a response of its
  // own origin, which it always is to its document, passes.
  // Upstream: services/network/cors/cors_url_loader.cc
  void CheckTimingAllowOrigin(const RequestEvent& event,
                              const url::Origin& document_origin) {
    const GURL url(event.url);
    // The Fetch Standard's response tainting stays once a hop has left the
    // document's origin: cors for a fetch, opaque for a script.
    timing_cross_origin_ =
        timing_cross_origin_ || !document_origin.IsSameOriginWith(url);
    if (timing_allow_failed_ || is_navigation()) {
      return;
    }
    if (std::optional<std::string> value =
            HeaderValue(event.headers, "Timing-Allow-Origin");
        value &&
        TimingAllowOriginAllows(
            *value, timing_tainted_ ? url::Origin() : document_origin)) {
      return;
    }
    timing_allow_failed_ = timing_cross_origin_;
  }

  // A redirect that the request follows: CorsURLLoader taints the origin of
  // a hop from another origin than the document's to yet another, and
  // DocumentLoadTiming::AddRedirect() counts it and whether it crossed
  // origins.
  void FollowRedirect(const RequestEvent& event,
                      const url::Origin& document_origin) {
    const GURL from(event.url);
    const GURL to(event.new_url);
    if (!url::IsSameOriginWith(from, to) &&
        !document_origin.IsSameOriginWith(from)) {
      timing_tainted_ = true;
    }
    ++redirect_count_;
    cross_origin_redirect_ =
        cross_origin_redirect_ || !url::IsSameOriginWith(from, to);
    last_redirect_end_ = event.load_timing.receive_headers_end;
    // CrossOriginOpenerPolicyStatus::SanitizeResponse() drops the COOP of
    // each of a navigation's responses at a URL that is not potentially
    // trustworthy, and says so on the console of the document it commits.
    if (is_navigation() && IgnoresCoop(from, PolicyHeadersOf(event.headers))) {
      ++coop_ignored_;
    }
  }
  // How many of the navigation's redirects had a COOP that was ignored.
  int coop_ignored() const { return coop_ignored_; }

  void set_job(base::WeakPtr<RunningRequest> job) { job_ = std::move(job); }
  // Lets the job go on without this Request, if it is still running, held to
  // the same checks, and still counting among the keepalive requests in
  // flight until it ends.
  // A body that streams from the document, of which the library does not
  // have all yet, fails from a later task, as Blink's BytesUploader answers
  // GetSize() with ERR_FAILED once its context is destroyed.
  void DetachJob() {
    if (job_) {
      job_->Detach(base::BindRepeating(
          [](ResponseChecks* checks, const KeepaliveShare*,
             const RequestEvent& event) { return checks->Check(event); },
          base::Owned(std::make_unique<ResponseChecks>(response_checks_)),
          base::Owned(std::move(keepalive_share_))));
      if (body_channel_ && !end_written_) {
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&RunningRequest::FailBody, job_));
      }
      job_.reset();
    }
  }
  void set_keepalive_share(std::unique_ptr<KeepaliveShare> share) {
    keepalive_share_ = std::move(share);
  }
  // The context that asked, or kNoDocument for the Session's navigation.
  uint64_t context() const { return context_; }
  void set_context(uint64_t context) { context_ = context; }
  void set_started() { started_ = true; }
  // The URL of the response, after any redirects, once it has started.
  const GURL& response_url() const { return response_url_; }
  void set_response_url(GURL url) { response_url_ = std::move(url); }
  void AddCredit(uint32_t bytes) { credit_ += bytes; }
  base::span<const uint8_t> TakeRead(size_t size) {
    credit_ -= static_cast<uint32_t>(size);
    return buffer_->first(size);
  }

  // Reads more of the body, unless a read is under way or the Page has given
  // no credit. A read that completes at once comes back from a later task, so
  // that a body in the cache does not read itself on the stack.
  void ReadIfCredited() {
    if (!started_ || reading_ || credit_ == 0 || !job_) {
      return;
    }
    reading_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&RunningRequest::ReadBody, job_));
  }

  // A body that streams from the Page, which the job reads from `channel` as
  // //net asks for it, as Chrome's network service reads a fetch()'s stream
  // from the data pipe that Blink's BytesUploader writes.
  void set_body_channel(scoped_refptr<RequestChannel> channel) {
    body_channel_ = std::move(channel);
  }
  bool streams_body() const { return body_channel_ != nullptr; }
  // Whether the Page has ended the body that streams.
  bool page_ended_body() const { return page_ended_body_; }
  // Whether `size` more bytes of the body fit in the credit that the library
  // gave the Page.
  bool BodyPartFits(size_t size) const { return size <= body_credit_; }
  // Bytes of the body that the Page sent, which wait for room in the channel.
  void AddBodyPart(std::string_view bytes) {
    body_credit_ -= static_cast<uint32_t>(bytes.size());
    body_waiting_.append(bytes);
    WriteBody();
  }
  // The Page's end of the body: all of it has come, or its stream errored,
  // which fails the request with ERR_FAILED from a later task, as
  // BytesUploader's answer of ERR_FAILED to GetSize() fails it in Chrome.
  // Upstream: third_party/blink/renderer/core/fetch/bytes_uploader.cc
  void EndBody(bool errored) {
    page_ended_body_ = true;
    if (errored) {
      body_waiting_.clear();
      if (job_) {
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&RunningRequest::FailBody, job_));
      }
      return;
    }
    WriteBody();
  }

  // RequestClient:
  int CheckResponse(const RequestEvent& event) override {
    return response_checks_.Check(event);
  }
  void OnResponseEvent(RequestEvent event) override {
    CheckTimingAllowOrigin(event, document_origin());
    // The job follows each redirect, and the Page sees only where it ends.
    if (event.type == MITTO_EVENT_REDIRECT_FOLLOWED) {
      FollowRedirect(event, document_origin());
    }
    if (event.type == MITTO_EVENT_RESPONSE_STARTED) {
      host_->OnResponseStarted(*this, std::move(event));
    }
  }
  net::IOBuffer* read_buffer() override { return buffer_.get(); }
  int read_size() const override { return static_cast<int>(credit_); }
  void OnBodyRead(size_t size) override {
    reading_ = false;
    host_->OnBodyRead(*this, size);
  }
  void OnTotals(const RequestTotals& totals) override { totals_ = totals; }
  void OnBodyWanted() override {
    // The channel's events are for a caller on another thread, and this one
    // has heard of its own.
    RequestEvent event;
    while (body_channel_->NextEvent(base::TimeDelta(), event) == net::OK) {
    }
    // //net's first read of the body is where Chrome's network service asks
    // the renderer to start reading the stream, with StartReading(), which
    // here is the Page's first credit, of one window.
    if (!body_started_) {
      body_started_ = true;
      GiveBodyCredit(kBodyWindow);
    }
    WriteBody();
  }
  void OnFinished(int net_error) override {
    // The job goes after this, and so does this Request.
    job_ = nullptr;
    host_->OnRequestFinished(*this, net_error);
  }

 private:
  // Writes what waits of the body into the channel, all at once as far as
  // the channel has room, as ChunkedDataPipeUploadDataStream reads what its
  // data pipe holds, then the end once the Page has ended it, and gives the
  // Page credit for what went. What the job no longer wants, as after a 303,
  // goes as well.
  // Upstream: services/network/chunked_data_pipe_upload_data_stream.cc
  void WriteBody() {
    bool wake = false;
    size_t taken = 0;
    if (!body_waiting_.empty()) {
      const int result =
          body_channel_->WriteBody(base::as_byte_span(body_waiting_), wake);
      // ERR_IO_PENDING: //net has yet to take the last write, and
      // OnBodyWanted() says when it has.
      DCHECK_NE(result, net::ERR_INVALID_ARGUMENT);
      if (result == 0) {
        taken = body_waiting_.size();
      } else if (result > 0) {
        taken = static_cast<size_t>(result);
      }
      body_waiting_.erase(0, taken);
    }
    if (body_waiting_.empty() && page_ended_body_ && !end_written_) {
      end_written_ = true;
      bool wake_for_end = false;
      body_channel_->EndBody(wake_for_end);
      wake = wake || wake_for_end;
    }
    if (taken > 0) {
      GiveBodyCredit(static_cast<uint32_t>(taken));
    }
    // //net's read of the body that waits goes on from a later task, since
    // this may run within it.
    if (wake && job_) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&RunningRequest::SendBody, job_));
    }
  }

  void GiveBodyCredit(uint32_t bytes) {
    body_credit_ += bytes;
    PageCredit credit;
    credit.number = number_;
    credit.bytes = bytes;
    host_->channel_->Send(EncodeRequestBodyCredit(credit));
  }

  const raw_ref<PageHost> host_;
  const uint64_t number_;
  const GURL first_url_;
  FetchParams navigation_fetch_;
  bool keeps_document_ = false;
  std::optional<Sniffing> sniffing_;
  ResponseChecks response_checks_;
  // A keepalive request's place among those in flight.
  std::unique_ptr<KeepaliveShare> keepalive_share_;
  uint64_t context_ = kNoDocument;
  GURL response_url_;
  const base::TimeTicks start_;
  const scoped_refptr<net::IOBufferWithSize> buffer_;
  base::WeakPtr<RunningRequest> job_;
  // How many more bytes of the body the Page has room for.
  uint32_t credit_ = kBodyWindow;
  bool started_ = false;
  bool reading_ = false;
  // For the Performance Timeline.
  bool timing_allow_failed_ = false;
  bool timing_cross_origin_ = false;
  bool timing_tainted_ = false;
  uint32_t redirect_count_ = 0;
  bool cross_origin_redirect_ = false;
  int coop_ignored_ = 0;
  base::TimeTicks last_redirect_end_;
  RequestTotals totals_;
  // A body that streams from the Page: its channel, what the Page sent that
  // waits for room there, how much more the Page may send, whether //net has
  // started reading it, and whether the Page, and then the channel, has ended
  // it.
  scoped_refptr<RequestChannel> body_channel_;
  std::string body_waiting_;
  uint32_t body_credit_ = 0;
  bool body_started_ = false;
  bool page_ended_body_ = false;
  bool end_written_ = false;
};

// One storage area that the Page has bound, and its observer, which the Page
// numbers: Chrome's StorageArea receiver and StorageAreaObserver remote for a
// renderer. A new observer takes the place of the old one, whose events the
// Page no longer reads, as a new Mojo pipe would.
class PageHost::StorageArea {
 public:
  StorageArea(PageHost& host, StorageAreaImpl& area, PageStorageKind kind)
      : host_(host), area_(area), kind_(kind) {
    area_->Bind();
  }

  StorageArea(const StorageArea&) = delete;
  StorageArea& operator=(const StorageArea&) = delete;

  ~StorageArea() {
    observer_.reset();
    area_->Unbind();
  }

  StorageAreaImpl& area() { return *area_; }
  PageStorageKind kind() const { return kind_; }

  // Puts an observer numbered `number` in the place of the one before, and
  // returns it for the area to add when it should. An area of sessionStorage
  // has none: Chrome's SessionStorageAreaImpl tells a renderer of no change,
  // since the renderer is the source of truth for its own, and only of a
  // deletion that the browser starts, which a Session never does.
  base::WeakPtr<StorageAreaObserver> ReplaceObserver(uint64_t number) {
    observer_.reset();
    if (kind_ == PageStorageKind::kSession) {
      return nullptr;
    }
    observer_ = std::make_unique<Observer>(*host_, *area_, number);
    return observer_->GetWeakPtr();
  }

 private:
  class Observer final : public StorageAreaObserver {
   public:
    Observer(PageHost& host, StorageAreaImpl& area, uint64_t number)
        : host_(host), area_(area), number_(number) {}

    ~Observer() override { area_->RemoveObserver(this); }

    base::WeakPtr<StorageAreaObserver> GetWeakPtr() {
      return weak_factory_.GetWeakPtr();
    }

    // StorageAreaObserver:
    void KeyChanged(const std::string& key,
                    const std::string& new_value,
                    const std::optional<std::string>& old_value,
                    const PageStorageSource& source) override {
      PageStorageEvent event = Event(source);
      event.key = key;
      event.new_value = new_value;
      event.old_value = old_value;
      Send(PageMessage::kStorageKeyChanged, event);
    }
    void KeyChangeFailed(const std::string& key,
                         const PageStorageSource& source) override {
      PageStorageEvent event = Event(source);
      event.key = key;
      Send(PageMessage::kStorageKeyChangeFailed, event);
    }
    void KeyDeleted(const std::string& key,
                    const std::optional<std::string>& old_value,
                    const PageStorageSource& source) override {
      PageStorageEvent event = Event(source);
      event.key = key;
      event.old_value = old_value;
      Send(PageMessage::kStorageKeyDeleted, event);
    }
    void AllDeleted(bool was_nonempty,
                    const PageStorageSource& source) override {
      PageStorageEvent event = Event(source);
      event.was_nonempty = was_nonempty;
      Send(PageMessage::kStorageAllDeleted, event);
    }

   private:
    PageStorageEvent Event(const PageStorageSource& source) const {
      PageStorageEvent event;
      event.observer = number_;
      event.source = source;
      return event;
    }

    void Send(PageMessage type, const PageStorageEvent& event) {
      // An event fits, in parts if it is larger than a frame, since the area
      // holds no more than its quota and the writer's URL came in a frame.
      if (!host_->channel_ || !StorageEventFits(event)) {
        return;
      }
      for (std::string& frame : EncodeStorageEvent(type, event)) {
        host_->channel_->Send(std::move(frame));
      }
    }

    const raw_ref<PageHost> host_;
    const raw_ref<StorageAreaImpl> area_;
    const uint64_t number_;
    base::WeakPtrFactory<Observer> weak_factory_{this};
  };

  const raw_ref<PageHost> host_;
  const raw_ref<StorageAreaImpl> area_;
  const PageStorageKind kind_;
  std::unique_ptr<Observer> observer_;
};

PageHost::PageHost(scoped_refptr<PageEvents> events,
                   scoped_refptr<PageProcess> process,
                   ReadyRequests& ready,
                   uintptr_t handle,
                   PageNetwork& network)
    : events_(std::move(events)),
      process_(std::move(process)),
      ready_(ready),
      handle_(handle),
      network_(network) {}

PageHost::~PageHost() {
  indexed_db_.reset();
  cache_storage_.reset();
  AbandonRequests();
  ReleaseStorage();
}

bool PageHost::Start(PageChannelEnd ours, PageOptions options) {
  channel_ = PageChannel::Start(std::move(ours), *this);
  if (!channel_) {
    return false;
  }
  options_ = std::move(options);
  options_.start.connection = ConnectionOf(GURL(url::kAboutBlankURL));
  // The Page reads these in order, so nothing waits for its hello.
  channel_->Send(EncodeHello());
  channel_->Send(EncodeStart(options_.start));
  if (options_.url) {
    // What session.get(url) sends: a navigation that the browser started, from
    // no page.
    const PageRequestTemplate& navigation =
        options_.requests.at(PageRequestKind::kNavigation);
    RequestOptions navigation_options = navigation.options;
    navigation_options.fetch->initiator = std::nullopt;
    StartRequest(kNavigationRequest, kNoDocument, url::Origin(), *options_.url,
                 navigation.headers, std::move(navigation_options),
                 std::nullopt, nullptr,
                 FetchChecks{.is_navigation = true, .top_level = true});
  }
  return true;
}

void PageHost::StartRequest(uint64_t number,
                            uint64_t context_id,
                            const url::Origin& document_origin,
                            GURL url,
                            net::HttpRequestHeaders headers,
                            RequestOptions options,
                            std::optional<std::vector<char>> body,
                            scoped_refptr<RequestChannel> body_channel,
                            FetchChecks checks) {
  auto request =
      std::make_unique<Request>(*this, number, document_origin, url, checks);
  request->set_context(context_id);
  if (body_channel) {
    request->set_body_channel(body_channel);
  }
  if (checks.keepalive) {
    request->set_keepalive_share(std::make_unique<KeepaliveShare>(
        network_->keepalive_in_flight(), keepalive_in_flight_,
        KeepaliveBytes(url, headers)));
  }
  if (checks.is_navigation && options.fetch) {
    request->set_navigation_fetch(*options.fetch);
  }
  Request& client = *request;
  requests_.emplace(number, std::move(request));
  client.set_job(network_->StartPageRequest(
      client, std::move(url), std::move(headers), std::move(options),
      std::move(body), std::move(body_channel)));
}

void PageHost::OnRequest(PageRequest asked, uint64_t context_id) {
  if (asked.number <= last_request_number_) {
    Refuse(base::StrCat(
        {"a request numbered ", base::NumberToString(asked.number),
         ", after one numbered ", base::NumberToString(last_request_number_)}));
    return;
  }
  last_request_number_ = asked.number;
  if (requests_.size() >= kMaxPageRequests) {
    Refuse(base::StrCat({"more than ", base::NumberToString(kMaxPageRequests),
                         " requests at once"}));
    return;
  }
  const Context& context = contexts_.at(context_id);
  // Only a document navigates, and a frame's navigation names the frame's
  // document that the new one replaces, whose parent the frame tree counts
  // from.
  if (context.is_worker && (asked.kind == PageRequestKind::kNavigation ||
                            asked.kind == PageRequestKind::kIframe)) {
    Refuse("a navigation that a worker starts");
    return;
  }
  uint64_t frame_parent = kNoDocument;
  if (asked.kind == PageRequestKind::kIframe) {
    const auto target = contexts_.find(asked.target);
    if (target == contexts_.end() || target->second.is_worker ||
        target->second.parent == kNoDocument) {
      Refuse("a frame's navigation whose target is not a frame's document");
      return;
    }
    frame_parent = target->second.parent;
  }
  // The URLs of the frame's ancestors' documents, which each redirect of its
  // navigation is held to.
  std::vector<GURL> frame_ancestor_urls;
  for (uint64_t ancestor = frame_parent; ancestor != kNoDocument;
       ancestor = contexts_.at(ancestor).parent) {
    frame_ancestor_urls.push_back(contexts_.at(ancestor).url);
  }
  // A module of a module worker's top-level graph is of destination worker in
  // mode cors, where the worker's own script, which its creator asks for, is
  // of mode same-origin. The worker asks for it, and Blink fetches it through
  // the worker's outside settings, its creator's: from the creator's origin,
  // with the creator's site for cookies, and with a module script's lines.
  // The creator stays while the worker does.
  // Upstream: third_party/blink/renderer/core/workers/worker_global_scope.cc
  const bool outside_settings = asked.kind == PageRequestKind::kWorker &&
                                asked.mode == PageRequestMode::kCors;
  if (outside_settings && !context.is_worker) {
    Refuse("a module of a worker's graph that no worker asks for");
    return;
  }
  const uint64_t settings_id = outside_settings ? context.parent : context_id;
  const url::Origin& document_origin = contexts_.at(settings_id).origin;
  // A frame's navigation is a navigation too, as Chrome's browser makes a
  // NavigationRequest of it, but the caller hears only of the top-level
  // frame's.
  const bool top_level = asked.kind == PageRequestKind::kNavigation;
  const bool is_navigation =
      top_level || asked.kind == PageRequestKind::kIframe;
  if (top_level) {
    PageEvent started;
    started.type = MITTO_PAGE_EVENT_NAVIGATION_STARTED;
    started.url = asked.url;
    Push(std::move(started));
  }
  const PageRequestKind template_kind =
      outside_settings ? PageRequestKind::kModule : asked.kind;
  const auto with_body = options_.requests_with_body.find(template_kind);
  const auto without_body = options_.requests.find(template_kind);
  const bool has_body = asked.body || asked.streams_body;
  const PageRequestTemplate* const request_template =
      has_body && with_body != options_.requests_with_body.end()
          ? &with_body->second
      : without_body != options_.requests.end() ? &without_body->second
                                                : nullptr;
  GURL url(asked.url);
  // The Page names only a referrer of its document's origin, as Fetch makes
  // any other the document's own URL, or the final URL of an earlier request
  // of the context that has finished, as for what a module or a style sheet
  // of another origin imports.
  GURL referrer;
  if (!asked.referrer.empty()) {
    referrer = GURL(asked.referrer);
    // A document's opaque origin, which a sandbox or a data: URL gives it, is
    // made for one whose URLs it may name.
    const bool own = context.is_worker
                         ? document_origin.IsSameOriginWith(referrer)
                         : document_origin.GetTupleOrPrecursorTupleIfOpaque() ==
                               url::SchemeHostPort(referrer);
    if (!referrer.is_valid() ||
        (!own && !context.response_urls.contains(referrer))) {
      Refuse(
          "a request whose referrer is neither a URL of its document's origin "
          "nor that of a response it has had");
      return;
    }
  }
  // The script's own lines, each name once, with its values joined, in the
  // order the names first come, as Fetch combines them.
  std::vector<std::pair<std::string, std::string>> lines;
  for (const std::string_view line :
       base::SplitStringPiece(asked.headers, "\n", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(": ");
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = line.substr(colon + 2);
    const auto same = std::ranges::find_if(lines, [&name](const auto& entry) {
      return base::EqualsCaseInsensitiveASCII(entry.first, name);
    });
    if (same == lines.end()) {
      lines.emplace_back(name, value);
    } else {
      base::StrAppend(&same->second, {", ", value});
    }
  }
  // These fail as such a request fails in Chrome, and the Page goes on.
  int net_error = net::OK;
  if (!url.is_valid()) {
    net_error = net::ERR_INVALID_URL;
  } else if (!url.SchemeIsHTTPOrHTTPS()) {
    net_error = net::ERR_UNKNOWN_URL_SCHEME;
  } else if (!request_template) {
    net_error = net::ERR_NOT_IMPLEMENTED;
  } else if (document_origin.opaque() && !context.is_worker &&
             context.parent == kNoDocument) {
    // The Page's first document, about:blank, which the Session did not load,
    // has no Initiator that a request can name. A frame's document of an
    // opaque origin, such as a data: URL's or Chrome's error page, asks as
    // that origin, as a worker of a data: URL does.
    net_error = net::ERR_ACCESS_DENIED;
  } else if (asked.mode == PageRequestMode::kSameOrigin &&
             !document_origin.IsSameOriginWith(url)) {
    // A cross-origin fetch of mode same-origin fails, as in Chrome. Another
    // has the CORS check held to its response, after the preflight that the
    // library makes when it needs one.
    net_error = net::ERR_FAILED;
  }
  if (net_error != net::OK) {
    PageResponseEnded ended;
    ended.number = asked.number;
    ended.net_error = net_error;
    channel_->Send(EncodeResponseEnded(ended));
    if (top_level) {
      PushNavigated(asked.url, 0, net_error);
    }
    return;
  }
  // The script's lines take the places of the Persona's lines of their names,
  // and the rest follow, as a caller's headers do. A Content-Type that the
  // script does not give goes, as a caller's body without one does.
  net::HttpRequestHeaders headers = request_template->headers;
  RequestOptions options = request_template->options;
  options.method = asked.method;
  FillContextFetch(settings_id, frame_parent, *options.fetch);
  if (outside_settings) {
    options.fetch->destination = "worker";
    options.fetch->from_worker = true;
  }
  // A navigation that the document starts sends Sec-Fetch-User only with the
  // user's activation, which the browser checks the renderer's word for.
  options.fetch->has_user_activation = asked.user_activation;
  options.validate_cache = asked.reload;
  ApplyPageRequest(asked, headers, options);
  if (context.is_worker) {
    PutOriginAfterUserAgent(headers);
  }
  if (!asked.referrer.empty()) {
    options.fetch->referrer_source = referrer;
    options.fetch->no_referrer_source = false;
  }
  if (has_body && !std::ranges::any_of(lines, [](const auto& entry) {
        return base::EqualsCaseInsensitiveASCII(
            entry.first, net::HttpRequestHeaders::kContentType);
      })) {
    headers.RemoveHeader(net::HttpRequestHeaders::kContentType);
  }
  for (const auto& [name, value] : lines) {
    headers.SetHeader(name, value);
    options.fetch->fixed_headers.insert(base::ToLowerASCII(name));
  }
  // One keepalive request too many, or too large, fails as in Chrome.
  if (asked.keepalive) {
    const KeepaliveInFlight& in_session = *network_->keepalive_in_flight();
    if (in_session.requests >= kMaxKeepaliveRequests ||
        keepalive_in_flight_->requests >= kMaxKeepaliveRequestsPerPage ||
        keepalive_in_flight_->bytes + KeepaliveBytes(url, headers) >
            kMaxKeepaliveBytesPerPage) {
      PageResponseEnded ended;
      ended.number = asked.number;
      ended.net_error = net::ERR_INSUFFICIENT_RESOURCES;
      channel_->Send(EncodeResponseEnded(ended));
      return;
    }
  }
  const RequestMode options_mode = options.fetch->mode;
  const CredentialsMode options_credentials = options.fetch->credentials_mode;
  std::optional<std::vector<char>> body;
  if (asked.body) {
    body.emplace(asked.body->begin(), asked.body->end());
  }
  // A fetch() whose body is a stream: a chunked upload, which //net makes
  // only over HTTP/2 or HTTP/3, as Chrome's ChunkedDataPipeUploadDataStream
  // allows no HTTP/1.1, of mode cors-with-forced-preflight, as Blink's
  // FetchManager gives it.
  // Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
  scoped_refptr<RequestChannel> body_channel;
  if (asked.streams_body) {
    body_channel = base::MakeRefCounted<RequestChannel>();
    body_channel->OpenBody(std::nullopt);
    options.stream_body = true;
    options.fetch->force_preflight = true;
  }
  if (asked.kind == PageRequestKind::kIframe) {
    frame_navigation_responses_[asked.number] = {.parent = frame_parent,
                                                 .target = asked.target};
  }
  std::optional<CrossOriginEmbedderPolicy> creator_embedder_policy;
  if (asked.kind == PageRequestKind::kWorker && !outside_settings) {
    worker_scripts_[asked.number] = {.creator = context_id};
    creator_embedder_policy = context.coep;
  }
  StartRequest(
      asked.number, context_id, document_origin, std::move(url),
      std::move(headers), std::move(options), std::move(body),
      std::move(body_channel),
      FetchChecks{
          .cors = options_mode == RequestMode::kCors,
          .same_origin = options_mode == RequestMode::kSameOrigin,
          .no_cors = options_mode == RequestMode::kNoCors,
          .credentials = options_credentials == CredentialsMode::kInclude,
          .keepalive = asked.keepalive,
          .is_navigation = is_navigation,
          .top_level = top_level,
          .creator_embedder_policy = std::move(creator_embedder_policy),
          .embedder_policy = contexts_.at(settings_id).coep,
          .frame_ancestor_urls = std::move(frame_ancestor_urls)});
}

void PageHost::OnCancel(uint64_t number) {
  // A navigation that the Page let go after its response came, before it
  // made a document of it, as when another takes its place, has ended too.
  if (const auto pending = navigation_responses_.find(number);
      pending != navigation_responses_.end()) {
    PushNavigated(pending->second.url.spec(), 0, net::ERR_ABORTED);
    navigation_responses_.erase(pending);
  }
  // So has a frame's, whose response the Page made no document of.
  frame_navigation_responses_.erase(number);
  const auto found = requests_.find(number);
  if (found == requests_.end()) {
    // The request may have ended since the Page let it go.
    if (number > last_request_number_) {
      Refuse(base::StrCat({"a cancel of request ", base::NumberToString(number),
                           ", which the Page has not asked for"}));
    }
    return;
  }
  // A navigation that the Page let go before its response, as when another
  // takes its place, has ended, and so has one whose response is held back
  // while it is sniffed, as one that the Page let go after its response came.
  if (found->second->is_top_level_navigation()) {
    if (!found->second->started()) {
      PushNavigated(found->second->first_url().possibly_invalid_spec(), 0,
                    net::ERR_ABORTED);
    } else if (const RequestEvent* const held =
                   found->second->held_response()) {
      PushNavigated(held->url, 0, net::ERR_ABORTED);
    }
  }
  // A navigation that keeps its document has told the Page that it ended
  // already, and its download goes on without the frame, as Chrome's
  // download manager takes it over, so the cancel changes nothing.
  if (found->second->keeps_document()) {
    return;
  }
  // The Request abandons its job as it goes.
  requests_.erase(found);
  PageResponseEnded ended;
  ended.number = number;
  ended.net_error = net::ERR_ABORTED;
  channel_->Send(EncodeResponseEnded(ended));
}

void PageHost::OnCredit(const PageCredit& credit) {
  const auto found = requests_.find(credit.number);
  if (found == requests_.end()) {
    // The request may have ended since the Page took those bytes.
    if (credit.number > last_request_number_) {
      Refuse(base::StrCat({"credit for request ",
                           base::NumberToString(credit.number),
                           ", which the Page has not asked for"}));
    }
    return;
  }
  Request& request = *found->second;
  if (credit.bytes > kBodyWindow - request.credit()) {
    Refuse(base::StrCat({"credit for more of request ",
                         base::NumberToString(credit.number),
                         " than the library sent"}));
    return;
  }
  request.AddCredit(credit.bytes);
  request.ReadIfCredited();
}

PageHost::Request* PageHost::RequestWithBodyStream(uint64_t number,
                                                   std::string_view what) {
  const auto found = requests_.find(number);
  if (found == requests_.end()) {
    // The request may have ended since the Page sent this.
    if (number > last_request_number_) {
      Refuse(base::StrCat({what, " of request ", base::NumberToString(number),
                           ", which the Page has not asked for"}));
    }
    return nullptr;
  }
  Request& request = *found->second;
  if (!request.streams_body() || request.page_ended_body()) {
    Refuse(base::StrCat({what, " of request ", base::NumberToString(number),
                         ", whose body does not stream or has ended"}));
    return nullptr;
  }
  return &request;
}

void PageHost::OnRequestBodyPart(const PageRequestBodyPart& part) {
  Request* const request =
      RequestWithBodyStream(part.number, "a part of the body");
  if (!request) {
    return;
  }
  if (!request->BodyPartFits(part.bytes.size())) {
    Refuse(base::StrCat({"more of the body of request ",
                         base::NumberToString(part.number),
                         " than the library gave credit for"}));
    return;
  }
  request->AddBodyPart(part.bytes);
}

void PageHost::OnRequestBodyEnded(const PageRequestBodyEnded& ended) {
  if (Request* const request =
          RequestWithBodyStream(ended.number, "the end of the body")) {
    request->EndBody(ended.errored);
  }
}

void PageHost::OnResponseStarted(Request& request, RequestEvent event) {
  request.set_started();
  request.set_response_url(GURL(event.url));
  if (request.is_navigation() && !MakesDocument(event)) {
    DropNavigation(request, event);
    return;
  }
  if (request.is_navigation()) {
    if (std::string mime_type = MimeTypeOf(event);
        ShouldSniffContent(event, mime_type)) {
      request.HoldForSniffing(std::move(event), std::move(mime_type));
      request.ReadIfCredited();
      return;
    }
  }
  StartResponse(request, event);
}

void PageHost::DropNavigation(Request& request, const RequestEvent& event) {
  // NavigationRequest::OnResponseStarted() drops a navigation whose response
  // is no document, and the Page keeps its document. A download goes on
  // without it.
  request.set_keeps_document();
  if (request.is_top_level_navigation()) {
    PushNavigated(event.url, event.status, net::ERR_ABORTED);
  }
  frame_navigation_responses_.erase(request.number());
  // The Page's frame lets its navigation go.
  if (channel_) {
    PageResponseEnded ended;
    ended.number = request.number();
    ended.net_error = net::ERR_ABORTED;
    channel_->Send(EncodeResponseEnded(ended));
  }
  request.ReadIfCredited();
}

void PageHost::FinishSniffing(Request& request) {
  Request::Sniffing sniffing = request.TakeSniffing();
  // A sniffed type that Chrome does not show, such as application/octet-stream
  // for text/plain that holds binary bytes, makes it a download, as
  // download_utils::MustDownload() decides from the type that the network
  // service sniffed. Any other sniffed type reaches the Page as the
  // response's own.
  if (!IsSupportedMimeType(sniffing.sniffed)) {
    // The Page never gets what was read.
    request.AddCredit(static_cast<uint32_t>(sniffing.body.size()));
    DropNavigation(request, sniffing.response);
    return;
  }
  StartResponse(request, sniffing.response);
  if (channel_ && !sniffing.body.empty()) {
    channel_->Send(EncodeResponseBody(request.number(), sniffing.body));
  }
}

void PageHost::StartResponse(Request& request, const RequestEvent& event) {
  // What Chrome's browser makes of a top-level navigation's COOP and COEP.
  std::optional<TopLevelDocumentPolicies> policies;
  if (request.is_top_level_navigation()) {
    const GURL url(event.url);
    const PolicyHeaders headers = PolicyHeadersOf(event.headers);
    policies = TopLevelDocumentPoliciesOf(url, headers);
    // The document that the Page makes of it, which it tells the library of,
    // and the caller hears of then.
    navigation_responses_[request.number()] = {
        .url = url,
        .status = event.status,
        .coep = policies->coep,
        .coop_ignored =
            request.coop_ignored() + (IgnoresCoop(url, headers) ? 1 : 0),
        .fetch = request.navigation_fetch(),
        .referrer = event.referrer};
  }
  // Where a frame's document, or a worker, that the Page makes of it comes
  // from.
  if (const auto frame = frame_navigation_responses_.find(request.number());
      frame != frame_navigation_responses_.end()) {
    frame->second.url = GURL(event.url);
  }
  if (const auto script = worker_scripts_.find(request.number());
      script != worker_scripts_.end()) {
    script->second.url = GURL(event.url);
    script->second.coep = EmbedderPolicyOf(PolicyHeadersOf(event.headers));
  }
  if (channel_ && policies) {
    // The document's agent cluster, which Chrome's browser commits it with.
    PageIsolation isolation;
    isolation.navigation = request.number();
    isolation.isolation = policies->cross_origin_isolated
                              ? PageCrossOriginIsolation::kCoopAndCoep
                              : PageCrossOriginIsolation::kNone;
    channel_->Send(EncodeIsolation(isolation));
  }
  if (channel_) {
    PageResponseStarted response;
    response.number = request.number();
    response.status = event.status;
    response.redirected = event.redirected;
    response.cors = request.cors();
    if (request.is_navigation()) {
      response.navigation_start =
          Microseconds(request.number() == kNavigationRequest &&
                               !options_.navigation_start.is_null()
                           ? options_.navigation_start
                           : request.start());
      response.connection = ConnectionOf(GURL(event.url));
    }
    response.was_fetched_via_spdy = event.was_fetched_via_spdy;
    response.was_alpn_negotiated = event.was_alpn_negotiated;
    response.url = event.url;
    response.status_text = event.status_text;
    // //net names every protocol, but a response that it never set them on,
    // as from a cache entry without them, says so.
    response.connection_info =
        event.protocol.empty() ? kUnknownProtocol : event.protocol;
    response.alpn_negotiated_protocol = event.alpn_negotiated_protocol.empty()
                                            ? kUnknownProtocol
                                            : event.alpn_negotiated_protocol;
    response.headers = HeadersForPage(event.headers);
    response.timing = PageLoadTimingOf(event.load_timing);
    response.timing_allow_passed = request.timing_allow_passed();
    response.opaque = request.opaque();
    response.validated = event.validated;
    response.received_bytes = event.received_bytes;
    response.redirect_count = request.redirect_count();
    response.cross_origin_redirect = request.cross_origin_redirect();
    response.restarted_for_critical_ch = event.restarted_for_critical_ch;
    response.first_request_start = Microseconds(event.first_request_start);
    response.last_redirect_end = Microseconds(request.last_redirect_end());
    response.critical_ch_restart = Microseconds(event.critical_ch_restart);
    if (request.is_navigation()) {
      response.referrer = event.referrer;
    }
    channel_->Send(EncodeResponseStarted(response));
  }
  request.ReadIfCredited();
}

PageConnection PageHost::ConnectionOf(const GURL& url) const {
  const NetworkQuality* const quality = network_->GetNetworkQuality();
  if (!quality) {
    return PageConnection();
  }
  // The document's site, as a subresource of its page takes it
  // (FetchRules::AddClientHints()). about:blank's is opaque.
  const RendererNetworkQuality renderer = GetRendererNetworkQuality(
      *quality, net::SchemefulSite(url::Origin::Create(url)), url);
  return {.rtt = renderer.rtt, .downlink = renderer.downlink};
}

void PageHost::OnBodyRead(Request& request, size_t size) {
  const base::span<const uint8_t> bytes = request.TakeRead(size);
  if (request.keeps_document()) {
    // A download's body goes nowhere.
    request.AddCredit(static_cast<uint32_t>(size));
    request.ReadIfCredited();
    return;
  }
  if (request.sniffing()) {
    // What is read while sniffing waits, counted against the Page's credit,
    // until the sniffing decides whether the Page gets it.
    if (request.Sniff(bytes, /*ended=*/false)) {
      FinishSniffing(request);
    } else {
      request.ReadIfCredited();
    }
    return;
  }
  if (channel_) {
    channel_->Send(
        EncodeResponseBody(request.number(), base::as_string_view(bytes)));
  }
  request.ReadIfCredited();
}

void PageHost::OnRequestFinished(Request& request, int net_error) {
  const uint64_t number = request.number();
  // A body that ends, or fails, while sniffing ends the sniffing with what
  // it gave.
  if (request.sniffing()) {
    request.Sniff({}, /*ended=*/true);
    FinishSniffing(request);
  }
  // A frame's navigation, or a worker's script, that ends with no response
  // gives the Page nothing to make a context of.
  if (!request.started()) {
    frame_navigation_responses_.erase(number);
    worker_scripts_.erase(number);
  }
  if (request.keeps_document()) {
    // The Page heard that it ended when its response started.
  } else if (request.is_navigation() && !request.started()) {
    // The Page's frame lets the navigation go: the top-level frame keeps its
    // document, and another commits Chrome's error page.
    if (request.is_top_level_navigation()) {
      PushNavigated(request.first_url().possibly_invalid_spec(), 0,
                    net_error == net::OK ? net::ERR_FAILED : net_error);
    }
    if (channel_) {
      PageResponseEnded ended;
      ended.number = number;
      ended.net_error = net_error == net::OK ? net::ERR_FAILED : net_error;
      channel_->Send(EncodeResponseEnded(ended));
    }
  } else if (channel_) {
    // A response that has come whole is one the context may name as the
    // referrer of what it imports.
    if (net_error == net::OK && request.started()) {
      if (const auto context = contexts_.find(request.context());
          context != contexts_.end()) {
        context->second.response_urls.insert(request.response_url());
      }
    }
    PageResponseEnded ended;
    ended.number = number;
    ended.net_error = net_error;
    const RequestTotals& totals = request.totals();
    ended.encoded_body_size = totals.raw_body_bytes;
    ended.received_bytes = totals.received_bytes;
    ended.completion_time = Microseconds(totals.completion_time);
    channel_->Send(EncodeResponseEnded(ended));
  }
  requests_.erase(number);
}

void PageHost::AbandonRequests() {
  // A keepalive request goes on without its Page, and the Session finishes
  // it. Each other Request abandons its job as it goes.
  for (auto& [number, request] : requests_) {
    if (request->checks().keepalive) {
      request->DetachJob();
    }
  }
  requests_.clear();
}

// static
bool PageHost::DocumentHasCookies(const GURL& document_url) {
  return document_url.is_valid() && document_url.SchemeIsHTTPOrHTTPS();
}

// static
std::optional<net::CookiePartitionKey> PageHost::FramePartitionKey(
    const url::Origin& top_frame_origin,
    const url::Origin& frame_origin,
    const net::SiteForCookies& site_for_cookies) {
  // RenderFrameHostImpl::GetIsolationInfoForSubresources(), which a frame's
  // RestrictedCookieManager gets.
  // Upstream: content/browser/renderer_host/render_frame_host_impl.cc
  const net::IsolationInfo isolation_info = net::IsolationInfo::Create(
      net::IsolationInfo::RequestType::kOther, top_frame_origin, frame_origin,
      site_for_cookies);
  return net::CookiePartitionKey::FromNetworkIsolationKey(
      isolation_info.network_isolation_key(), isolation_info.site_for_cookies(),
      net::SchemefulSite(frame_origin), isolation_info.IsMainFrameRequest());
}

// What RestrictedCookieManager does for document.cookie, with the SCRIPT role
// and always for the document that the Session loaded, never for a URL that
// the Page names.
// Upstream: services/network/restricted_cookie_manager.cc
void PageHost::OnGetCookies(uint64_t question, const CookieContext& context) {
  if (!DocumentHasCookies(context.url)) {
    channel_->Send(EncodeCookies(question, ""));
    return;
  }
  const net::SiteForCookies& site_for_cookies = context.site_for_cookies;
  net::CookieOptions options;
  options.set_exclude_httponly();
  options.set_same_site_cookie_context(
      net::cookie_util::ComputeSameSiteContextForScriptGet(
          context.url, site_for_cookies, /*initiator=*/std::nullopt,
          /*force_ignore_site_for_cookies=*/false));
  network_->cookie_store().GetCookieListWithOptionsAsync(
      context.url, options,
      net::CookiePartitionKeyCollection(context.partition_key),
      base::BindOnce(&PageHost::SendCookies, weak_factory_.GetWeakPtr(),
                     question));
}

void PageHost::SendCookies(uint64_t question,
                           const net::CookieAccessResultList& cookies,
                           const net::CookieAccessResultList& excluded) {
  if (channel_) {
    channel_->Send(EncodeCookies(
        question, net::CanonicalCookie::BuildCookieLine(cookies)));
  }
}

void PageHost::OnSetCookie(const std::string& line,
                           const CookieContext& context) {
  if (!DocumentHasCookies(context.url)) {
    return;
  }
  const std::optional<net::CookiePartitionKey> partition_key =
      context.partition_key;
  net::CookieInclusionStatus status;
  std::unique_ptr<net::CanonicalCookie> cookie = net::CanonicalCookie::Create(
      context.url, line, base::Time::Now(), /*server_time=*/std::nullopt,
      partition_key, net::CookieSourceType::kScript, &status);
  if (!cookie) {
    return;
  }
  const net::SiteForCookies& site_for_cookies = context.site_for_cookies;
  net::CookieOptions options;
  options.set_exclude_httponly();
  options.set_same_site_cookie_context(
      net::cookie_util::ComputeSameSiteContextForScriptSet(
          context.url, site_for_cookies,
          /*force_ignore_site_for_cookies=*/false));
  network_->cookie_store().SetCanonicalCookieAsync(
      std::move(cookie), context.url, options, base::DoNothing(),
      /*cookie_access_result=*/std::nullopt);
}

namespace {

// CookieChangeEvent::ToCookieListItem()'s same-site mode: the access's
// effective one, or else the cookie's own, and none for an unspecified one.
PageCookieSameSite SameSiteOf(const net::CanonicalCookie& cookie,
                              net::CookieEffectiveSameSite effective) {
  switch (effective) {
    case net::CookieEffectiveSameSite::STRICT_MODE:
      return PageCookieSameSite::kStrict;
    case net::CookieEffectiveSameSite::LAX_MODE:
    case net::CookieEffectiveSameSite::LAX_MODE_ALLOW_UNSAFE:
      return PageCookieSameSite::kLax;
    case net::CookieEffectiveSameSite::NO_RESTRICTION:
      return PageCookieSameSite::kNoRestriction;
    case net::CookieEffectiveSameSite::UNDEFINED:
    case net::CookieEffectiveSameSite::COUNT:
      break;
  }
  switch (cookie.SameSite()) {
    case net::CookieSameSite::STRICT_MODE:
      return PageCookieSameSite::kStrict;
    case net::CookieSameSite::LAX_MODE:
      return PageCookieSameSite::kLax;
    case net::CookieSameSite::NO_RESTRICTION:
      return PageCookieSameSite::kNoRestriction;
    case net::CookieSameSite::UNSPECIFIED:
      return PageCookieSameSite::kNone;
  }
}

PageCookie ToPageCookie(const net::CanonicalCookie& cookie,
                        net::CookieEffectiveSameSite effective) {
  PageCookie page_cookie;
  page_cookie.name = cookie.Name();
  page_cookie.value = cookie.Value();
  page_cookie.domain = cookie.Domain();
  page_cookie.path = cookie.Path();
  if (!cookie.ExpiryDate().is_null()) {
    page_cookie.expires =
        (cookie.ExpiryDate() - base::Time::UnixEpoch()).InMicroseconds();
  }
  page_cookie.secure = cookie.SecureAttribute();
  page_cookie.same_site = SameSiteOf(cookie, effective);
  page_cookie.partitioned = cookie.IsPartitioned();
  return page_cookie;
}

net::CookieSameSite ToNetSameSite(PageCookieSameSite same_site) {
  switch (same_site) {
    case PageCookieSameSite::kLax:
      return net::CookieSameSite::LAX_MODE;
    case PageCookieSameSite::kNoRestriction:
      return net::CookieSameSite::NO_RESTRICTION;
    case PageCookieSameSite::kStrict:
    case PageCookieSameSite::kNone:
      return net::CookieSameSite::STRICT_MODE;
  }
}

// MakeOptionsForGet() and MakeOptionsForSet() for the SCRIPT role.
net::CookieOptions ScriptOptions(const GURL& url,
                                 const net::SiteForCookies& site_for_cookies,
                                 bool set) {
  net::CookieOptions options;
  options.set_exclude_httponly();
  options.set_same_site_cookie_context(
      set ? net::cookie_util::ComputeSameSiteContextForScriptSet(
                url, site_for_cookies,
                /*force_ignore_site_for_cookies=*/false)
          : net::cookie_util::ComputeSameSiteContextForScriptGet(
                url, site_for_cookies, /*initiator=*/std::nullopt,
                /*force_ignore_site_for_cookies=*/false));
  return options;
}

}  // namespace

// RestrictedCookieManager::GetAllForUrl(): the document's cookies that a
// script may read, of the name asked for, or all of them.
void PageHost::OnCookieStoreGet(PageCookieStoreGet get,
                                const CookieContext& context) {
  if (!DocumentHasCookies(context.url)) {
    SendCookieStoreCookies(get, {}, {});
    return;
  }
  network_->cookie_store().GetCookieListWithOptionsAsync(
      context.url,
      ScriptOptions(context.url, context.site_for_cookies, /*set=*/false),
      net::CookiePartitionKeyCollection(context.partition_key),
      base::BindOnce(&PageHost::SendCookieStoreCookies,
                     weak_factory_.GetWeakPtr(), std::move(get)));
}

void PageHost::SendCookieStoreCookies(
    const PageCookieStoreGet& get,
    const net::CookieAccessResultList& cookies,
    const net::CookieAccessResultList& excluded) {
  if (!channel_) {
    return;
  }
  PageCookieStoreCookies answer;
  answer.question = get.question;
  for (const net::CookieWithAccessResult& item : cookies) {
    if (!item.access_result.status.IsInclude() ||
        (!get.all && item.cookie.Name() != get.name)) {
      continue;
    }
    answer.cookies.push_back(
        ToPageCookie(item.cookie, item.access_result.effective_same_site));
  }
  channel_->Send(EncodeCookieStoreCookies(answer));
}

// RestrictedCookieManager::SetCanonicalCookie(), both halves: the cookie is
// made as the Page asked, made again from storage with the times of now, and
// stored if the store takes it.
void PageHost::OnCookieStoreSet(PageCookieStoreSet set,
                                const CookieContext& context) {
  const uint64_t question = set.question;
  const auto answer = [this, question](bool stored) {
    if (channel_) {
      channel_->Send(EncodeCookieStoreStored(question, stored));
    }
  };
  if (!DocumentHasCookies(context.url)) {
    answer(false);
    return;
  }
  const std::optional<net::CookiePartitionKey> partition_key =
      set.partitioned ? context.partition_key : std::nullopt;
  const base::Time expiry =
      base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(set.expiry));
  net::CookieInclusionStatus status;
  std::unique_ptr<net::CanonicalCookie> cookie =
      net::CanonicalCookie::CreateSanitizedCookie(
          context.url, set.name, set.value, set.domain, set.path,
          /*creation_time=*/base::Time(), expiry,
          /*last_access_time=*/base::Time(), /*secure=*/true,
          /*http_only=*/false, ToNetSameSite(set.same_site),
          net::COOKIE_PRIORITY_MEDIUM, partition_key, &status);
  // IsCookieDomainValid(): the Page checks the domain first, so one that
  // fails here comes from a Page that does not.
  if ((cookie && !cookie->IsDomainMatch(context.url.host())) ||
      status.HasExclusionReason(net::CookieInclusionStatus::ExclusionReason::
                                    EXCLUDE_DOMAIN_MISMATCH)) {
    Refuse("a CookieStore cookie for another domain");
    return;
  }
  if (!cookie) {
    answer(false);
    return;
  }
  // A URL with a leading dot, which no document has.
  if (!net::cookie_util::DomainIsHostOnly(context.url.host())) {
    status.AddExclusionReason(
        net::CookieInclusionStatus::ExclusionReason::EXCLUDE_INVALID_DOMAIN);
  }
  if (!status.IsInclude()) {
    answer(false);
    return;
  }
  const url::Origin origin = url::Origin::Create(context.url);
  const base::Time now = base::Time::Now();
  std::unique_ptr<net::CanonicalCookie> sanitized =
      net::CanonicalCookie::FromStorage(
          cookie->Name(), cookie->Value(), cookie->Domain(), cookie->Path(),
          now, cookie->ExpiryDate(), now, now, cookie->SecureAttribute(),
          cookie->IsHttpOnly(), cookie->SameSite(), cookie->Priority(),
          cookie->PartitionKey(),
          GURL::SchemeIsCryptographic(origin.scheme())
              ? net::CookieSourceScheme::kSecure
              : net::CookieSourceScheme::kNonSecure,
          origin.port(), cookie->SourceType(),
          net::CanonicalCookieFromStorageCallSite::kRestrictedCookieManager);
  // FromStorage() checks less than IsCanonical() does.
  if (!sanitized || !sanitized->IsCanonical()) {
    answer(false);
    return;
  }
  network_->cookie_store().SetCanonicalCookieAsync(
      std::move(sanitized), origin.GetURL(),
      ScriptOptions(context.url, context.site_for_cookies, /*set=*/true),
      base::BindOnce(
          [](base::WeakPtr<PageHost> host, uint64_t question,
             net::CookieAccessResult result) {
            if (host && host->channel_) {
              host->channel_->Send(
                  EncodeCookieStoreStored(question, result.status.IsInclude()));
            }
          },
          weak_factory_.GetWeakPtr(), question),
      net::CookieAccessResult(status));
}

// RestrictedCookieManager::AddChangeListener(), and dropping the listener's
// pipe.
void PageHost::OnCookieStoreListen(bool listen,
                                   uint64_t document,
                                   const CookieContext& context) {
  if (!listen) {
    cookie_store_listeners_.erase(document);
    return;
  }
  if (cookie_store_listeners_.contains(document) ||
      !DocumentHasCookies(context.url)) {
    return;
  }
  CookieStoreListener& listener = cookie_store_listeners_[document];
  listener.context = context;
  // The dispatcher stops calling once the subscription goes, which this
  // owns.
  listener.subscription =
      network_->cookie_store().GetChangeDispatcher().AddCallbackForUrl(
          context.url, context.partition_key,
          base::BindRepeating(&PageHost::OnCookieStoreChange,
                              base::Unretained(this), document));
}

// RestrictedCookieManager::Listener::OnCookieChange() and
// CookieChangeEvent::ToEventInfo(): a change that the document's script may
// see, but for an overwrite, which the insertion after it stands for.
void PageHost::OnCookieStoreChange(uint64_t document,
                                   const net::CookieChangeInfo& change) {
  const auto listener = cookie_store_listeners_.find(document);
  if (!channel_ || listener == cookie_store_listeners_.end()) {
    return;
  }
  const CookieContext& context = listener->second.context;
  const net::CookieAccessDelegate* const delegate =
      network_->cookie_store().cookie_access_delegate();
  const bool trustworthy =
      delegate && delegate->ShouldTreatUrlAsTrustworthy(context.url);
  if (!change.cookie
           .IncludeForRequestURL(
               context.url,
               ScriptOptions(context.url, context.site_for_cookies,
                             /*set=*/false),
               net::CookieAccessParams(change.access_result.access_semantics,
                                       change.access_result.scope_semantics,
                                       trustworthy))
           .status.IsInclude()) {
    return;
  }
  PageCookieStoreChange page_change;
  page_change.document = document;
  switch (change.cause) {
    case net::CookieChangeCause::INSERTED:
    case net::CookieChangeCause::INSERTED_NO_VALUE_CHANGE_OVERWRITE:
      page_change.deleted = false;
      break;
    case net::CookieChangeCause::EXPLICIT:
    case net::CookieChangeCause::UNKNOWN_DELETION:
    case net::CookieChangeCause::EXPIRED:
    case net::CookieChangeCause::EVICTED:
    case net::CookieChangeCause::EXPIRED_OVERWRITE:
      page_change.deleted = true;
      break;
    case net::CookieChangeCause::OVERWRITE:
    case net::CookieChangeCause::INSERTED_NO_CHANGE_OVERWRITE:
      return;
  }
  page_change.cookie =
      ToPageCookie(change.cookie, change.access_result.effective_same_site);
  channel_->Send(EncodeCookieStoreChange(page_change));
}

void PageHost::OnBroadcast(const PageBroadcast& broadcast,
                           uint64_t context_id) {
  // Only a context of an origin that is not opaque sends one: one of an opaque
  // origin shares its channels with no other Page's.
  const Context& context = contexts_.at(context_id);
  if (!DocumentHasCookies(ContextUrl(context)) || context.origin.opaque()) {
    Refuse(
        "a broadcast from a Page with no document of an origin that "
        "another can share");
    return;
  }
  // Nor does one under a top-level document of an opaque origin, whose key no
  // other Page's context has.
  if (context.storage_key) {
    network_->BroadcastFromPage(*this, *context.storage_key, broadcast);
  }
}

void PageHost::OnCacheStorage(PageCacheStorageCall call,
                              uint64_t context_id,
                              const GURL& document_url) {
  if (!cache_storage_) {
    cache_storage_ = std::make_unique<PageCacheStorage>(
        network_->cache_storage(), network_->storage_quota(),
        base::BindRepeating(
            [](base::WeakPtr<PageHost> host, std::string frame) {
              if (host && host->channel_ && !host->refused_by_library_) {
                host->channel_->Send(std::move(frame));
              }
            },
            weak_factory_.GetWeakPtr()),
        base::BindRepeating(
            [](base::WeakPtr<PageHost> host, std::string why) {
              if (host && !host->refused_by_library_) {
                host->Refuse(std::move(why));
              }
            },
            weak_factory_.GetWeakPtr()),
        next_cache_storage_number_);
  }
  std::optional<std::string> storage_key =
      SecureContextStorageKey(context_id, document_url);
  bool bucket_live = true;
  switch (call.op) {
    case CacheStorageCallOp::kOpen:
    case CacheStorageCallOp::kHas:
    case CacheStorageCallOp::kDelete:
    case CacheStorageCallOp::kKeys:
    case CacheStorageCallOp::kMatch:
      // CacheStorage's calls go to the bucket they name.
      if (storage_key) {
        std::optional<PageStorageBuckets::Target> target =
            LocateBucket(call.bucket, context_id, *storage_key);
        if (!target) {
          return;
        }
        storage_key = std::move(target->bucket_key);
        bucket_live = target->live;
      }
      break;
    default:
      break;
  }
  cache_storage_->OnCall(std::move(call), context_id, storage_key, bucket_live);
}

std::optional<std::string> PageHost::SecureContextStorageKey(
    uint64_t context_id,
    const GURL& url) {
  // GlobalCacheStorage::CanCreateCacheStorage(), and the [SecureContext] of
  // caches and of the Storage Buckets API: an origin that is not opaque, of a
  // potentially trustworthy URL.
  if (!url.SchemeIs(url::kHttpsScheme) && !net::IsLocalhost(url)) {
    return std::nullopt;
  }
  return ContextSerializedStorageKey(context_id);
}

std::optional<PageStorageBuckets::Target> PageHost::LocateBucket(
    uint64_t bucket,
    uint64_t context_id,
    const std::string& storage_key) {
  if (bucket == 0) {
    network_->storage_quota().EnsureDefaultBucket(storage_key);
    return PageStorageBuckets::Target{storage_key, true};
  }
  if (!storage_buckets_) {
    Refuse(base::StrCat({"a call of the storage of bucket ",
                         base::NumberToString(bucket),
                         ", which the Page was not given"}));
    return std::nullopt;
  }
  return storage_buckets_->Locate(bucket, context_id);
}

void PageHost::OnStorageBuckets(PageStorageBucketCall call,
                                uint64_t context_id,
                                const GURL& document_url) {
  if (!storage_buckets_) {
    storage_buckets_ = std::make_unique<PageStorageBuckets>(
        network_->storage_quota(),
        base::BindRepeating(
            [](base::WeakPtr<PageHost> host, std::string frame) {
              if (host && host->channel_ && !host->refused_by_library_) {
                host->channel_->Send(std::move(frame));
              }
            },
            weak_factory_.GetWeakPtr()),
        base::BindRepeating(
            [](base::WeakPtr<PageHost> host, std::string why) {
              if (host && !host->refused_by_library_) {
                host->Refuse(std::move(why));
              }
            },
            weak_factory_.GetWeakPtr()));
  }
  storage_buckets_->OnCall(std::move(call), context_id,
                           SecureContextStorageKey(context_id, document_url));
}

void PageHost::OnIndexedDb(PageIndexedDbCall call,
                           uint64_t context_id,
                           const GURL& document_url) {
  if (!indexed_db_) {
    indexed_db_ = std::make_unique<PageIndexedDb>(
        network_->indexed_db(),
        base::BindRepeating(
            [](base::WeakPtr<PageHost> host, std::string frame) {
              if (host && host->channel_ && !host->refused_by_library_) {
                host->channel_->Send(std::move(frame));
              }
            },
            weak_factory_.GetWeakPtr()),
        base::BindRepeating(
            [](base::WeakPtr<PageHost> host, std::string why) {
              if (host && !host->refused_by_library_) {
                host->Refuse(std::move(why));
              }
            },
            weak_factory_.GetWeakPtr()));
  }
  // IDBFactory's check that the origin may reach databases: not an opaque
  // one, which a Page's documents only have outside HTTP and HTTPS.
  std::optional<std::string> storage_key =
      ContextSerializedStorageKey(context_id);
  bool bucket_live = true;
  switch (call.op) {
    case IndexedDbCallOp::kGetDatabaseInfo:
    case IndexedDbCallOp::kOpen:
    case IndexedDbCallOp::kDeleteDatabase:
      // A factory's calls go to the bucket they name.
      if (storage_key) {
        std::optional<PageStorageBuckets::Target> target =
            LocateBucket(call.bucket, context_id, *storage_key);
        if (!target) {
          return;
        }
        storage_key = std::move(target->bucket_key);
        bucket_live = target->live;
      }
      break;
    default:
      break;
  }
  indexed_db_->OnCall(std::move(call), context_id, storage_key, bucket_live);
}

std::optional<std::string> PageHost::ContextSerializedStorageKey(uint64_t id) {
  const Context& context = contexts_.at(id);
  if (!DocumentHasCookies(ContextUrl(context)) || context.origin.opaque()) {
    return std::nullopt;
  }
  if (context.storage_key) {
    return context.storage_key->Serialize();
  }
  // A frame under a top-level document of an opaque origin, such as a data:
  // URL's, whose key has that origin's nonce, which no other top-level
  // document has, so that what it keeps stays among the frames under that one
  // document.
  uint64_t top = id;
  while (contexts_.at(top).parent != kNoDocument) {
    top = contexts_.at(top).parent;
  }
  const auto [nonce, added] = opaque_top_nonces_.try_emplace(top);
  if (added) {
    nonce->second = base::UnguessableToken::Create();
  }
  return SerializeUnderOpaqueTop(context.origin, nonce->second);
}

// static
std::optional<StorageKey> PageHost::CalculateStorageKey(
    const Context& context) {
  // RenderFrameHostImpl::CalculateStorageKey(), with
  // net::features::kThirdPartyStoragePartitioning on, as in Chrome.
  // Upstream: content/browser/renderer_host/render_frame_host_impl.cc
  if (!DocumentHasCookies(ContextUrl(context)) || context.origin.opaque()) {
    return std::nullopt;
  }
  const FrameTree& tree = context.frame_tree;
  if (tree.is_top_level) {
    return StorageKey::CreateFirstParty(context.origin);
  }
  if (tree.top_frame_origin.opaque()) {
    return std::nullopt;
  }
  return StorageKey::Create(
      context.origin, net::SchemefulSite(tree.top_frame_origin),
      tree.cross_site ? StorageKey::AncestorChainBit::kCrossSite
                      : StorageKey::AncestorChainBit::kSameSite);
}

void PageHost::ReleaseStorage() {
  storage_areas_.clear();
  session_storage_.reset();
  opaque_top_storage_.clear();
}

// What Chrome's browser checks when a renderer binds an area, in
// DOMStorageContextWrapper, and the binding, in LocalStorageImpl or the
// renderer's session storage namespace.
// Upstream: content/browser/dom_storage/dom_storage_context_wrapper.cc
void PageHost::OnStorageBind(const PageStorageBind& bind, uint64_t document) {
  if (bind.area <= last_storage_area_ ||
      bind.observer <= last_storage_observer_) {
    Refuse("a storage area or observer numbered as one before it");
    return;
  }
  last_storage_area_ = bind.area;
  last_storage_observer_ = bind.observer;
  if (storage_areas_.size() >= kMaxPageStorageAreas) {
    Refuse(
        base::StrCat({"more than ", base::NumberToString(kMaxPageStorageAreas),
                      " storage areas at once"}));
    return;
  }
  // A document of an opaque origin throws before it asks.
  const Context& context = contexts_.at(document);
  if (!DocumentHasCookies(ContextUrl(context)) || context.origin.opaque()) {
    Refuse("a storage area of a document that has no storage key");
    return;
  }
  StorageAreaImpl* area = nullptr;
  if (const std::optional<StorageKey>& storage_key = context.storage_key) {
    if (bind.kind == PageStorageKind::kLocal) {
      area = &network_->storage_context().LocalStorageArea(*storage_key);
    } else {
      if (!session_storage_) {
        session_storage_ = std::make_unique<SessionStorageNamespace>();
      }
      area = &session_storage_->Area(*storage_key);
    }
  } else {
    // A frame under a top-level document of an opaque origin, such as a data:
    // URL's. Chrome keys its storage by that origin, which no other top-level
    // document has, so that what it writes stays among the frames under that
    // one document. Here it stays in memory, for as long as the Page.
    uint64_t top = document;
    while (contexts_.at(top).parent != kNoDocument) {
      top = contexts_.at(top).parent;
    }
    std::unique_ptr<SessionStorageNamespace>& storage =
        opaque_top_storage_[{top, bind.kind}];
    if (!storage) {
      storage = std::make_unique<SessionStorageNamespace>();
    }
    area = &storage->Area(StorageKey::CreateFirstParty(context.origin));
  }
  auto storage_area = std::make_unique<StorageArea>(*this, *area, bind.kind);
  if (base::WeakPtr<StorageAreaObserver> observer =
          storage_area->ReplaceObserver(bind.observer)) {
    area->AddObserver(observer.get());
  }
  storage_areas_.emplace(bind.area, std::move(storage_area));
}

void PageHost::OnStorageGetAll(const PageStorageGetAll& get_all) {
  const auto found = storage_areas_.find(get_all.area);
  if (found == storage_areas_.end()) {
    Refuse("a question for a storage area that is not bound");
    return;
  }
  if (get_all.observer <= last_storage_observer_) {
    Refuse("a storage observer numbered as one before it");
    return;
  }
  last_storage_observer_ = get_all.observer;
  StorageArea& area = *found->second;
  area.area().GetAll(
      area.ReplaceObserver(get_all.observer),
      base::BindOnce(
          [](base::WeakPtr<PageHost> host, uint64_t area,
             std::vector<std::pair<std::string, std::string>> entries) {
            if (!host || !host->channel_) {
              return;
            }
            for (std::string& frame : EncodeStorageEntries(area, entries)) {
              host->channel_->Send(std::move(frame));
            }
          },
          weak_factory_.GetWeakPtr(), get_all.area));
}

void PageHost::OnStorageMutation(PageMessage type,
                                 PageStorageMutation mutation) {
  const auto found = storage_areas_.find(mutation.area);
  if (found == storage_areas_.end()) {
    Refuse("a change to a storage area that is not bound");
    return;
  }
  StorageArea& area = *found->second;
  if (type != PageMessage::kStorageDeleteAll &&
      !IsStorageAsBlinkWrites(
          area.kind(), mutation.key,
          type == PageMessage::kStoragePut
              ? std::optional<std::string_view>(mutation.value)
              : std::nullopt)) {
    Refuse("a change to storage of a key or value that Blink does not write");
    return;
  }
  switch (type) {
    case PageMessage::kStoragePut:
      area.area().Put(mutation.key, mutation.value, mutation.source);
      return;
    case PageMessage::kStorageDelete:
      area.area().Delete(mutation.key, mutation.source);
      return;
    case PageMessage::kStorageDeleteAll: {
      base::WeakPtr<StorageAreaObserver> new_observer;
      if (mutation.observer != 0) {
        if (mutation.observer <= last_storage_observer_) {
          Refuse("a storage observer numbered as one before it");
          return;
        }
        last_storage_observer_ = mutation.observer;
        new_observer = area.ReplaceObserver(mutation.observer);
      }
      area.area().DeleteAll(mutation.source, std::move(new_observer));
      return;
    }
    default:
      NOTREACHED();
  }
}

void PageHost::OnStorageUnbind(uint64_t area) {
  if (storage_areas_.erase(area) == 0) {
    Refuse("a storage area that is not bound");
  }
}

void PageHost::OnNetworkHint(const PageNetworkHint& hint, uint64_t document) {
  // The Page names an origin, as Blink's WebPrescientNetworkingImpl hands the
  // browser a url::SchemeHostPort, and only of HTTP or HTTPS, the only ones
  // that PreconnectManager takes.
  const GURL url(hint.url);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() ||
      url != url.DeprecatedGetOriginAsURL()) {
    Refuse("a network hint that is not an HTTP or HTTPS origin");
    return;
  }
  // GetPendingIsolationInfoForSubresources() of the document's frame.
  const Context& context = contexts_.at(document);
  const FrameTree& tree = context.frame_tree;
  const net::SiteForCookies site_for_cookies =
      tree.cross_site ? net::SiteForCookies()
                      : net::SiteForCookies::FromOrigin(tree.top_frame_origin);
  const net::NetworkAnonymizationKey key =
      net::IsolationInfo::Create(net::IsolationInfo::RequestType::kOther,
                                 tree.top_frame_origin, context.origin,
                                 site_for_cookies)
          .network_anonymization_key();
  PreconnectManager& preconnect_manager = network_->preconnect_manager();
  switch (hint.type) {
    case PageNetworkHintType::kPrefetchDns:
      preconnect_manager.StartPreresolveHost(url, key);
      return;
    case PageNetworkHintType::kPreconnect:
      preconnect_manager.StartPreconnectUrl(url, hint.allow_credentials, key);
      return;
  }
  NOTREACHED();
}

void PageHost::DeliverBroadcast(const StorageKey& storage_key,
                                const PageBroadcast& broadcast) {
  // The Page hands it to the channels of its contexts of that storage key, if
  // it has one. A message too large for a frame with its key stays out, as one
  // too large for a frame stays in the Page that posts it.
  if (!channel_ || ended_ || refused_by_library_ ||
      std::ranges::none_of(contexts_, [&storage_key](const auto& entry) {
        return entry.second.storage_key == storage_key;
      })) {
    return;
  }
  PageBroadcast received = broadcast;
  received.storage_key = storage_key.Serialize();
  if (!BroadcastReceivedFits(received)) {
    return;
  }
  channel_->Send(EncodeBroadcastReceived(received));
}

void PageHost::Evaluate(uint64_t call, std::string script) {
  if (ended_) {
    events_->PushCallAfterEnd(call);
    ready_->Add(handle_, MITTO_READY_PAGE);
    return;
  }
  calls_.insert(call);
  // Once the channel has closed, the call fails when the Page ends.
  if (channel_) {
    channel_->Send(EncodeEvaluate(call, script));
  }
}

void PageHost::EndWithSession() {
  if (ended_) {
    return;
  }
  AbandonRequests();
  ReleaseStorage();
  channel_.reset();
  // A put that waits for the rest of its body from the Page fails, so that
  // its cache, and the Session's storage, may close.
  cache_storage_.reset();
  // Its databases' connections close before the Session's storage does.
  indexed_db_.reset();
  End(MITTO_PAGE_ERROR_SESSION_DESTROYED, "the Session was destroyed");
}

PageHost::NavigationRecord::NavigationRecord() = default;
PageHost::NavigationRecord::NavigationRecord(NavigationRecord&&) = default;
PageHost::NavigationRecord& PageHost::NavigationRecord::operator=(
    NavigationRecord&&) = default;
PageHost::NavigationRecord::~NavigationRecord() = default;

void PageHost::OnDocumentCommitted(const PageDocumentCommitted& committed) {
  if (committed.document <= last_context_id_) {
    Refuse(base::StrCat({"a document numbered ",
                         base::NumberToString(committed.document),
                         ", after a context numbered ",
                         base::NumberToString(last_context_id_)}));
    return;
  }
  const bool top_level = committed.parent == kNoDocument;
  if (!top_level) {
    const auto parent = contexts_.find(committed.parent);
    if (parent == contexts_.end() || parent->second.is_worker) {
      Refuse(
          "a frame's document whose parent is not one of the Page's documents");
      return;
    }
  }
  Context context;
  context.parent = committed.parent;
  std::optional<int32_t> navigated_status;
  // How many of its navigation's responses had a COOP that was ignored.
  int coop_ignored = 0;
  // What a traversal to the document repeats.
  std::optional<NavigationRecord> record;
  // The frame's document that the new one replaces, if any.
  uint64_t replaced = kNoDocument;
  switch (committed.source) {
    case PageDocumentSource::kInitialEmpty:
      if (top_level) {
        // The Page's first document, before any other, whose origin is
        // opaque.
        if (last_context_id_ != kNoDocument) {
          Refuse("an initial about:blank after another document");
          return;
        }
      } else {
        // A frame's first document, which has its parent's origin,
        // embedder policy and cookie URL.
        const Context& parent = contexts_.at(committed.parent);
        context.origin = parent.origin;
        context.coep = parent.coep;
        context.cookie_url = CookieUrlOf(parent);
      }
      context.url = GURL(url::kAboutBlankURL);
      break;
    case PageDocumentSource::kNavigationResponse:
      if (top_level) {
        const auto found = navigation_responses_.find(committed.navigation);
        if (found == navigation_responses_.end()) {
          Refuse(base::StrCat({"a document of navigation ",
                               base::NumberToString(committed.navigation),
                               ", which has no response"}));
          return;
        }
        record.emplace();
        record->fetch = std::move(found->second.fetch);
        record->url = found->second.url;
        record->referrer = std::move(found->second.referrer);
        context.url = std::move(found->second.url);
        context.coep = found->second.coep;
        navigated_status = found->second.status;
        coop_ignored = found->second.coop_ignored;
        navigation_responses_.erase(found);
      } else {
        const auto found =
            frame_navigation_responses_.find(committed.navigation);
        if (found == frame_navigation_responses_.end() ||
            !found->second.url.is_valid()) {
          Refuse(base::StrCat({"a frame's document of navigation ",
                               base::NumberToString(committed.navigation),
                               ", which has no response"}));
          return;
        }
        if (found->second.parent != committed.parent) {
          Refuse("a frame's document under another parent than its frame's");
          return;
        }
        context.url = std::move(found->second.url);
        replaced = found->second.target;
        frame_navigation_responses_.erase(found);
      }
      context.origin = url::Origin::Create(context.url);
      // Blink's Document::CookieURL() is the URL it was made at, which a
      // navigation within it leaves, as the Page keeps it too.
      context.cookie_url = context.url;
      break;
    case PageDocumentSource::kBlob:
      // A blob: URL's document, until the library can tie it to a navigation
      // that the Page started and to a Blob that it holds, which #283 brings,
      // could take any origin that its claimed initiator has.
      Refuse("a blob: document, which no navigation of the library's backs");
      return;
    case PageDocumentSource::kAboutBlank:
    case PageDocumentSource::kSrcdoc:
    case PageDocumentSource::kErrorPage:
    case PageDocumentSource::kDataUrl: {
      // A document that a frame commits without the network takes the place
      // of the frame's document, under the same parent, as Chrome's browser
      // commits it in that frame. srcdoc, an error page and a data: URL's
      // document are only a frame's: the top-level frame keeps its document
      // when its navigation fails, and Chrome shows it no data: URL that a
      // page navigates it to.
      const auto replaces = contexts_.find(committed.replaces);
      if (replaces == contexts_.end() || replaces->second.is_worker ||
          replaces->second.parent != committed.parent ||
          (top_level && committed.source != PageDocumentSource::kAboutBlank)) {
        Refuse(
            "a document that does not take the place of its frame's document");
        return;
      }
      if (committed.source == PageDocumentSource::kAboutBlank) {
        // RenderFrameHostImpl::SetOriginDependentStateOfNewFrame() and
        // NavigationRequest::GetOriginToCommit(): about:blank takes the origin
        // of the document that started its navigation, and its policy
        // container, with its embedder policy, and its cookie URL, as they
        // were when the navigation started.
        const auto started = about_blank_navigations_.find(committed.replaces);
        if (started == about_blank_navigations_.end() ||
            started->second.kind != PageLocalNavigationKind::kAboutBlank ||
            started->second.initiator != committed.initiator) {
          Refuse("an about:blank whose navigation did not start");
          return;
        }
        context.origin = started->second.origin;
        context.coep = started->second.coep;
        context.cookie_url = started->second.cookie_url;
        about_blank_navigations_.erase(started);
        context.url = GURL(url::kAboutBlankURL);
      } else if (committed.source == PageDocumentSource::kSrcdoc) {
        // A srcdoc's are its parent's.
        const Context& parent = contexts_.at(committed.parent);
        context.origin = parent.origin;
        context.coep = parent.coep;
        context.cookie_url = CookieUrlOf(parent);
        context.url = GURL(url::kAboutSrcdocURL);
      } else if (committed.source == PageDocumentSource::kErrorPage) {
        // NavigationRequest::GetOriginToCommit(): an error page's origin is
        // opaque.
        context.url = GURL(kErrorPageUrl);
      } else {
        // So is a data: URL's, made for its initiator's, whose requests send no
        // Referer and no cookies. It takes the policy container, with the
        // embedder policy, of the document that started its navigation, as it
        // was when the navigation started, as a local URL's document does, and
        // keeps its URL, which a navigation to a fragment then changes.
        const auto started = about_blank_navigations_.find(committed.replaces);
        const GURL url(committed.url);
        if (started == about_blank_navigations_.end() ||
            started->second.kind != PageLocalNavigationKind::kDataUrl ||
            started->second.initiator != committed.initiator ||
            !url.is_valid() || !url.SchemeIs(url::kDataScheme)) {
          Refuse("a data: URL's document whose navigation did not start");
          return;
        }
        context.origin = url::Origin::Resolve(url, started->second.origin);
        context.coep = started->second.coep;
        about_blank_navigations_.erase(started);
        context.url = url;
      }
      replaced = committed.replaces;
      break;
    }
  }
  // NavigationRequest::GetOriginToCommit(): an opaque origin, made for the one
  // that the document would have, without allow-same-origin, which no document
  // under one that lacks it has.
  if (committed.sandboxed_origin) {
    if (top_level) {
      Refuse("a top-level document in a sandbox");
      return;
    }
    context.sandboxed_origin = true;
    context.origin = context.origin.DeriveNewOpaqueOrigin();
  } else if (!top_level && contexts_.at(committed.parent).sandboxed_origin) {
    Refuse("a document outside the sandbox of its parent");
    return;
  }
  last_context_id_ = committed.document;
  if (!top_level) {
    // It takes the place of its frame's document, and of everything under
    // that, whose messages all came before this.
    if (contexts_.contains(replaced)) {
      ForgetContexts(ContextsUnder(replaced));
    }
    AddContext(committed.document, std::move(context));
    return;
  }
  // It takes the place of the top-level document and everything under it,
  // whose messages all came before this. Their keepalive requests go on
  // without them, and the Page, which let them go, hears that they ended.
  LetKeepaliveRequestsGo(nullptr);
  contexts_.clear();
  frame_navigation_responses_.clear();
  about_blank_navigations_.clear();
  worker_scripts_.clear();
  const GURL url = context.url;
  AddContext(committed.document, std::move(context));
  // The records of the documents that the session history may go back to
  // stay, until the Page releases them, the oldest first past a bound.
  if (record) {
    records_.insert_or_assign(committed.document, std::move(*record));
    if (records_.size() > kMaxNavigationRecords) {
      records_.erase(records_.begin());
    }
  }
  // The old document's Cache API goes with it, as its pipes go with its
  // context in Chrome: its puts that wait for a body fail, and what it held
  // is let go. The new document's numbers go on from its.
  if (cache_storage_) {
    next_cache_storage_number_ = cache_storage_->NextNumber();
    cache_storage_.reset();
  }
  // Its IndexedDB's connections close, as a renderer's context closes them
  // when it goes.
  indexed_db_.reset();
  // The old document's CookieStore listened over a pipe that went with it.
  cookie_store_listeners_.clear();
  // The navigation's document has taken the place of the Page's, after every
  // message of the old one, its load among them.
  if (navigated_status) {
    PushNavigated(url.spec(), *navigated_status, net::OK);
  }
  // NavigationRequest's deferred console messages, which go to the console of
  // the document that it commits.
  for (int i = 0; i < coop_ignored; ++i) {
    PageEvent event;
    event.type = MITTO_PAGE_EVENT_CONSOLE;
    event.level = static_cast<int32_t>(ConsoleLevel::kError);
    event.message = std::string(kCoopIgnoredOnUntrustworthyOrigin);
    PushReport(std::move(event));
  }
}

void PageHost::OnAboutBlankNavigation(uint64_t document,
                                      uint64_t initiator_id,
                                      PageLocalNavigationKind kind) {
  // The browser's NavigationRequest takes the initiator's origin and policy
  // container as the navigation begins, so the about:blank or data: URL's
  // document has them though the initiator goes first. The newest navigation
  // of the frame takes the place of one that waits, and one that ended with
  // no document leaves nothing to commit.
  if (kind == PageLocalNavigationKind::kEnded) {
    about_blank_navigations_.erase(document);
    return;
  }
  const auto initiator = contexts_.find(initiator_id);
  if (contexts_.at(document).is_worker || initiator == contexts_.end() ||
      initiator->second.is_worker) {
    Refuse("an about:blank whose initiator is not one of the Page's documents");
    return;
  }
  AboutBlankNavigation& started = about_blank_navigations_[document];
  started.kind = kind;
  started.initiator = initiator_id;
  started.origin = initiator->second.origin;
  started.coep = initiator->second.coep;
  started.cookie_url = CookieUrlOf(initiator->second);
}

void PageHost::OnDocumentOpened(uint64_t document, uint64_t opener_id) {
  // DidOpenDocumentInputStream(): a document that one of another window and
  // of its origin opened takes the opener's URL, without its fragment, as the
  // browser takes it, and Blink's Document::open() its cookie URL, from the
  // library's own record of the opener, never from what the Page says.
  Context& context = contexts_.at(document);
  const auto opener = contexts_.find(opener_id);
  if (context.is_worker || opener == contexts_.end() ||
      opener->second.is_worker || opener_id == document ||
      opener->second.origin != context.origin) {
    Refuse("a document that no document of its origin opened");
    return;
  }
  const GURL url = opener->second.url.GetWithoutRef();
  context.cookie_url = CookieUrlOf(opener->second);
  // Document::open() sets no URL for an about:srcdoc opener.
  const bool changed = url != context.url && !url.IsAboutSrcdoc();
  if (changed) {
    context.url = url;
  }
  // The Page's URL is its top-level document's.
  if (!changed || context.parent != kNoDocument) {
    return;
  }
  if (const auto found = records_.find(document);
      found != records_.end() && url.IsAboutBlank()) {
    found->second.had_about_blank = true;
  }
  PageEvent event;
  event.type = MITTO_PAGE_EVENT_URL_CHANGED;
  event.url = url.spec();
  Push(std::move(event));
}

bool PageHost::MayHaveAboutBlankEntry(const Context& context) const {
  return context.parent == kNoDocument && !context.is_worker &&
         std::ranges::any_of(records_, [&context](const auto& entry) {
           return entry.second.had_about_blank &&
                  context.origin.IsSameOriginWith(entry.second.url);
         });
}

void PageHost::OnSameDocumentNavigation(uint64_t document,
                                        const std::string& url) {
  // RenderFrameHostImpl::ValidateURLAndOrigin() for a same-document URL: one
  // that the history API gives the document, as CanChangeToUrlForHistoryApi()
  // lets it. A frame's about:blank or srcdoc may have committed at another
  // such URL, such as about:blank?v=1, which the library does not hear of, so
  // one about:blank or about:srcdoc URL may follow another: the document
  // keeps its origin, and the Page holds it to its path and query. A URL that
  // document.open() gives a document, the library hears of with type 73, and a
  // traversal takes the top-level document, the only one with entries, to the
  // about:blank that it gave it or one before it of its origin, and from there
  // to any URL of its origin.
  Context& context = contexts_.find(document)->second;
  GURL& document_url = context.url;
  const GURL new_url(url);
  const bool is_entry_url =
      (new_url.IsAboutBlank() ||
       (document_url.IsAboutBlank() &&
        url::Origin::Create(new_url).IsSameOriginWith(context.origin))) &&
      MayHaveAboutBlankEntry(context);
  if (!new_url.is_valid() ||
      (!CanChangeToUrlForHistoryApi(new_url, context.origin, document_url) &&
       !is_entry_url &&
       !(document_url.IsAboutBlank() && new_url.IsAboutBlank()) &&
       !(document_url.IsAboutSrcdoc() && new_url.IsAboutSrcdoc()))) {
    Refuse("a same-document navigation to another origin");
    return;
  }
  document_url = new_url;
  // A traversal may go back to an entry of this URL.
  if (const auto found = records_.find(document); found != records_.end()) {
    NavigationRecord& record = found->second;
    record.had_about_blank |= new_url.IsAboutBlank();
    const auto same = std::ranges::find(record.same_document_urls, new_url);
    if (same != record.same_document_urls.end()) {
      record.same_document_url_bytes -= same->spec().size();
      record.same_document_urls.erase(same);
    }
    record.same_document_urls.push_back(new_url);
    record.same_document_url_bytes += new_url.spec().size();
    while (record.same_document_urls.size() > kMaxSameDocumentUrls ||
           record.same_document_url_bytes > kMaxSameDocumentUrlBytes) {
      record.same_document_url_bytes -=
          record.same_document_urls.front().spec().size();
      record.same_document_urls.erase(record.same_document_urls.begin());
    }
  }
  // The Page's URL is its top-level document's.
  if (context.parent != kNoDocument) {
    return;
  }
  PageEvent changed;
  changed.type = MITTO_PAGE_EVENT_URL_CHANGED;
  changed.url = new_url.spec();
  Push(std::move(changed));
}

void PageHost::OnTraversalRequest(const PageTraversalRequest& traversal,
                                  uint64_t document) {
  if (traversal.number <= last_request_number_) {
    Refuse(base::StrCat({"a traversal numbered ",
                         base::NumberToString(traversal.number),
                         ", after a request numbered ",
                         base::NumberToString(last_request_number_)}));
    return;
  }
  last_request_number_ = traversal.number;
  if (requests_.size() >= kMaxPageRequests) {
    Refuse(base::StrCat({"more than ", base::NumberToString(kMaxPageRequests),
                         " requests at once"}));
    return;
  }
  const Context& context = contexts_.at(document);
  if (context.is_worker || context.parent != kNoDocument) {
    Refuse("a traversal that the top-level document does not leave");
    return;
  }
  const auto found = records_.find(traversal.record);
  if (found == records_.end()) {
    Refuse("a traversal to a document that the library has no record of");
    return;
  }
  // An entry of the navigation goes with the policy of its own request, which
  // the record keeps, whatever document asks.
  if (!traversal.same_document_entry &&
      traversal.referrer_policy != PageReferrerPolicy::kDocument) {
    Refuse("a referrer policy for a traversal to a navigation's entry");
    return;
  }
  const NavigationRecord& record = found->second;
  const GURL url(traversal.url);
  PageEvent started;
  started.type = MITTO_PAGE_EVENT_NAVIGATION_STARTED;
  started.url = traversal.url;
  Push(std::move(started));
  // Only to a URL that the library saw the document take: its navigation's,
  // or one that a navigation within it gave it, which the library may have
  // let go of since, and then the traversal goes nowhere, with no request.
  if (traversal.same_document_entry
          ? !url.SchemeIsHTTPOrHTTPS() ||
                std::ranges::find(record.same_document_urls, url) ==
                    record.same_document_urls.end()
          : url != record.url) {
    PageResponseEnded ended;
    ended.number = traversal.number;
    ended.net_error = net::ERR_ABORTED;
    channel_->Send(EncodeResponseEnded(ended));
    PushNavigated(traversal.url, 0, net::ERR_ABORTED);
    return;
  }
  // NavigationControllerImpl::CreateNavigationRequestFromEntry(): the
  // entry's own Initiator, referrer and user activation, whichever document
  // asks. An entry that a navigation within the document made has the
  // document's origin as its Initiator, and the document's Referer.
  const PageRequestTemplate& navigation =
      options_.requests.at(PageRequestKind::kNavigation);
  RequestOptions options = navigation.options;
  *options.fetch = record.fetch;
  if (traversal.same_document_entry) {
    options.fetch->initiator = record.url;
    options.fetch->opaque_initiator.reset();
  }
  if (traversal.referrer_policy != PageReferrerPolicy::kDocument) {
    options.fetch->referrer_policy =
        ToNetReferrerPolicy(traversal.referrer_policy);
  }
  // The Referer that the navigation sent last, to which the policy of its
  // first request applies again, as Chrome's FrameNavigationEntry keeps the
  // committed referrer with the request's policy.
  if (record.referrer.empty()) {
    options.fetch->referrer_policy = net::ReferrerPolicy::NO_REFERRER;
    options.fetch->referrer_source.reset();
  } else {
    options.fetch->referrer_source = GURL(record.referrer);
  }
  options.skip_cache_validation = true;
  StartRequest(traversal.number, document, context.origin, url,
               navigation.headers, std::move(options), std::nullopt, nullptr,
               FetchChecks{.is_navigation = true, .top_level = true});
}

void PageHost::OnRecordsReleased(const std::vector<uint64_t>& records) {
  for (const uint64_t record : records) {
    records_.erase(record);
  }
}

void PageHost::PushNavigated(std::string url,
                             int32_t status,
                             int32_t net_error) {
  PageEvent navigated;
  navigated.type = MITTO_PAGE_EVENT_NAVIGATED;
  navigated.status = status;
  navigated.net_error = net_error;
  navigated.url = std::move(url);
  Push(std::move(navigated));
}

// static
GURL PageHost::ContextUrl(const Context& context) {
  // A frame's initial about:blank and a worker of a blob: URL speak for their
  // origin, which is HTTP or HTTPS where they have one to speak for.
  if (context.url.SchemeIsHTTPOrHTTPS() || context.origin.opaque()) {
    return context.url;
  }
  return context.origin.GetURL();
}

// static
GURL PageHost::CookieUrlOf(const Context& context) {
  return context.cookie_url.is_valid() ? context.cookie_url
                                       : ContextUrl(context);
}

const PageHost::Context* PageHost::ContextOf(uint64_t id, uint32_t type) {
  const auto found = contexts_.find(id);
  if (found == contexts_.end()) {
    Refuse(base::StrCat({"message type ", base::NumberToString(type),
                         " of context ", base::NumberToString(id),
                         ", which is not one of the Page's"}));
    return nullptr;
  }
  return &found->second;
}

std::set<uint64_t> PageHost::ContextsUnder(uint64_t id) const {
  std::set<uint64_t> under = {id};
  // Each context comes after its parent, as the Page numbers them.
  for (const auto& [context_id, context] : contexts_) {
    if (under.contains(context.parent)) {
      under.insert(context_id);
    }
  }
  return under;
}

void PageHost::ForgetContexts(const std::set<uint64_t>& ids) {
  LetKeepaliveRequestsGo(&ids);
  for (const uint64_t id : ids) {
    contexts_.erase(id);
    // Its CookieStore listened over a pipe that went with it.
    cookie_store_listeners_.erase(id);
    // Its frame's about:blank that waits commits in the place of no document.
    about_blank_navigations_.erase(id);
  }
  // Its Cache API's puts that wait for a body fail, and its caches and stored
  // bodies are let go, as its pipes go with it in Chrome, so that none holds
  // its cache.
  if (cache_storage_) {
    cache_storage_->ForgetContexts(ids);
  }
  // Its IndexedDB connections close, as a renderer closes a context's when
  // it goes.
  if (indexed_db_) {
    indexed_db_->ForgetContexts(ids);
  }
  // Its StorageBuckets' remotes go with it.
  if (storage_buckets_) {
    storage_buckets_->ForgetContexts(ids);
  }
  std::erase_if(frame_navigation_responses_, [&ids](const auto& entry) {
    return ids.contains(entry.second.parent) ||
           ids.contains(entry.second.target);
  });
  std::erase_if(worker_scripts_, [&ids](const auto& entry) {
    return ids.contains(entry.second.creator);
  });
}

void PageHost::LetKeepaliveRequestsGo(const std::set<uint64_t>* ids) {
  for (auto it = requests_.begin(); it != requests_.end();) {
    Request& request = *it->second;
    if (!request.checks().keepalive ||
        (ids && !ids->contains(request.context()))) {
      ++it;
      continue;
    }
    request.DetachJob();
    PageResponseEnded ended;
    ended.number = it->first;
    ended.net_error = net::ERR_ABORTED;
    channel_->Send(EncodeResponseEnded(ended));
    it = requests_.erase(it);
  }
}

void PageHost::FillContextFetch(uint64_t id,
                                uint64_t parent,
                                FetchParams& fetch) const {
  const Context& context = contexts_.at(id);
  // The Initiator is the context's URL, or the origin of a frame's initial
  // about:blank, which has its parent's, or the opaque origin of a worker of
  // a data: URL, whose requests send "null" as their Origin, as Chrome's do.
  if (context.origin.opaque()) {
    fetch.opaque_initiator = context.origin;
  } else {
    fetch.initiator = context.origin.IsSameOriginWith(context.url)
                          ? context.url
                          : context.origin.GetURL();
  }
  // Blink's OutgoingReferrer(): a srcdoc document's is its parent's, and a
  // worker's is its script's URL, of which a blob: or data: one gives none,
  // as about:blank gives none.
  const Context* source = &context;
  while (!source->is_worker && source->parent != kNoDocument &&
         source->url.IsAboutSrcdoc()) {
    source = &contexts_.at(source->parent);
  }
  // Nor does a document of an opaque origin, such as a sandboxed one's.
  if (!source->url.SchemeIsHTTPOrHTTPS() ||
      (!context.is_worker && context.origin.opaque())) {
    fetch.no_referrer_source = true;
  } else if (fetch.initiator && source->url != *fetch.initiator) {
    fetch.referrer_source = source->url;
  }
  // The frame tree, from the context's frame, or from a frame's parent for
  // its navigation, up to the top-level document, as Chrome's browser works
  // it out. A frame's navigation always names the top-level frame, since its
  // Initiator need not be it.
  const FrameTree& tree =
      contexts_.at(parent != kNoDocument ? parent : id).frame_tree;
  if (!tree.is_top_level || parent != kNoDocument ||
      fetch.opaque_initiator.has_value()) {
    fetch.top_frame_origin = tree.top_frame_origin;
  }
  // An opaque origin is of no site, so its requests have no site for
  // cookies, as the storage key of a worker of a data: URL gives them.
  fetch.cross_site_ancestor =
      tree.cross_site || fetch.opaque_initiator.has_value();
  fetch.cross_origin_ancestor = tree.cross_origin;
  fetch.from_worker = context.is_worker;
  fetch.credentialless_client =
      context.coep.value == CrossOriginEmbedderPolicyValue::kCredentialless;
}

GURL PageHost::TopDocumentUrl() const {
  for (const auto& [id, context] : contexts_) {
    if (context.parent == kNoDocument && !context.is_worker) {
      return context.url;
    }
  }
  return GURL();
}

PageHost::CookieStoreListener::CookieStoreListener() = default;
PageHost::CookieStoreListener::CookieStoreListener(CookieStoreListener&&) =
    default;
PageHost::CookieStoreListener& PageHost::CookieStoreListener::operator=(
    CookieStoreListener&&) = default;
PageHost::CookieStoreListener::~CookieStoreListener() = default;

PageHost::CookieContext::CookieContext() = default;
PageHost::CookieContext::CookieContext(const CookieContext&) = default;
PageHost::CookieContext& PageHost::CookieContext::operator=(
    const CookieContext&) = default;
PageHost::CookieContext::~CookieContext() = default;

PageHost::CookieContext PageHost::CookieContextOf(uint64_t id) const {
  const Context& context = contexts_.at(id);
  const FrameTree& tree = contexts_.at(id).frame_tree;
  CookieContext cookies;
  // A document of an opaque origin reads and writes none.
  cookies.url = context.origin.opaque() ? GURL() : CookieUrlOf(context);
  cookies.site_for_cookies =
      tree.cross_site ? net::SiteForCookies()
                      : net::SiteForCookies::FromOrigin(tree.top_frame_origin);
  cookies.partition_key = FramePartitionKey(
      tree.top_frame_origin, context.origin, cookies.site_for_cookies);
  return cookies;
}

void PageHost::OnWorkerStarted(const PageWorkerStarted& started) {
  if (started.worker <= last_context_id_) {
    Refuse(base::StrCat({"a worker numbered ",
                         base::NumberToString(started.worker),
                         ", after a context numbered ",
                         base::NumberToString(last_context_id_)}));
    return;
  }
  const Context* const creator = ContextOf(
      started.creator, static_cast<uint32_t>(PageMessage::kWorkerStarted));
  if (!creator) {
    return;
  }
  Context worker;
  worker.parent = started.creator;
  worker.is_worker = true;
  switch (started.source) {
    case PageWorkerSource::kScriptResponse: {
      // The script that its creator asked for, in mode same-origin, at the
      // URL that it ended at.
      const auto found = worker_scripts_.find(started.request);
      if (found == worker_scripts_.end() || !found->second.url.is_valid() ||
          found->second.creator != started.creator) {
        Refuse("a worker whose script is not a response to its creator");
        return;
      }
      worker.url = std::move(found->second.url);
      worker.origin = creator->origin;
      worker.coep = std::move(found->second.coep);
      worker_scripts_.erase(found);
      break;
    }
    case PageWorkerSource::kBlob: {
      const GURL url(started.url);
      if (!url.is_valid() || !url.SchemeIsBlob() ||
          !creator->origin.IsSameOriginWith(url::Origin::Create(url))) {
        Refuse("a worker of a blob: URL of another origin than its creator's");
        return;
      }
      worker.url = url;
      worker.origin = creator->origin;
      // A worker of a local scheme takes its creator's policies.
      worker.coep = creator->coep;
      break;
    }
    case PageWorkerSource::kData:
      // A data: URL's worker has an opaque origin, and no URL that a request
      // of its could start from. It takes its creator's policies.
      worker.coep = creator->coep;
      break;
  }
  last_context_id_ = started.worker;
  AddContext(started.worker, std::move(worker));
}

void PageHost::AddContext(uint64_t id, Context context) {
  // Neither its origin nor its place in the frame tree changes while it lasts.
  FrameTree& tree = context.frame_tree;
  if (context.parent == kNoDocument) {
    tree.top_frame_origin = context.origin;
  } else {
    tree = contexts_.at(context.parent).frame_tree;
  }
  if (context.parent != kNoDocument && !context.is_worker) {
    tree.is_top_level = false;
    tree.cross_site =
        tree.cross_site || net::SchemefulSite(context.origin) !=
                               net::SchemefulSite(tree.top_frame_origin);
    tree.cross_origin = tree.cross_origin ||
                        !context.origin.IsSameOriginWith(tree.top_frame_origin);
  }
  context.storage_key = CalculateStorageKey(context);
  contexts_.emplace(id, std::move(context));
}

void PageHost::OnContextGone(uint64_t context_id) {
  const Context* const context =
      ContextOf(context_id, static_cast<uint32_t>(PageMessage::kContextGone));
  if (!context) {
    return;
  }
  // The top-level document goes only when another takes its place.
  if (context->parent == kNoDocument && !context->is_worker) {
    Refuse("the top-level document going while the Page stays");
    return;
  }
  ForgetContexts(ContextsUnder(context_id));
}

void PageHost::OnFrame(uint32_t type, std::string payload) {
  base::expected<FrameFromPage, std::string> frame =
      ReadFrameFromPage(type, std::move(payload));
  if (!frame.has_value()) {
    Refuse(std::move(frame).error());
    return;
  }
  if (!hello_received_ && frame->type != PageMessage::kHello &&
      frame->type != PageMessage::kRefused) {
    Refuse(base::StrCat({"message type ", base::NumberToString(type),
                         " came before the hello"}));
    return;
  }
  // What a message of a context asks for is the context's, as the library
  // loaded it. A worker has no cookies of its own to read or write, no load
  // and no URL that changes.
  GURL document_url;
  const Context* context = nullptr;
  if (NamesDocument(frame->type)) {
    context = ContextOf(frame->document, type);
    if (!context) {
      return;
    }
    if (context->is_worker && frame->type != PageMessage::kRequest &&
        frame->type != PageMessage::kBroadcast &&
        frame->type != PageMessage::kCacheStorage &&
        frame->type != PageMessage::kIndexedDb &&
        frame->type != PageMessage::kStorageBuckets) {
      Refuse(base::StrCat({"message type ", base::NumberToString(type),
                           " of a worker, which only a document sends"}));
      return;
    }
    document_url = ContextUrl(*context);
  }
  switch (frame->type) {
    case PageMessage::kHello:
      if (hello_received_) {
        Refuse("a second hello");
        return;
      }
      if (frame->version != kPageProtocolVersion) {
        Refuse(base::StrCat({"the Page speaks version ",
                             base::NumberToString(frame->version),
                             " of the messages, and the library speaks ",
                             base::NumberToString(kPageProtocolVersion)}));
        return;
      }
      hello_received_ = true;
      return;
    case PageMessage::kRefused:
      // The Page ends after this.
      refused_by_page_ = std::move(frame->reason);
      WaitForExit();
      return;
    case PageMessage::kEvaluated: {
      PageEvaluated& evaluated = frame->evaluated;
      if (calls_.erase(evaluated.call) == 0) {
        Refuse(base::StrCat({"a result of call ",
                             base::NumberToString(evaluated.call),
                             ", which is not in progress"}));
        return;
      }
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_EVALUATED;
      event.call = evaluated.call;
      if (evaluated.failed) {
        event.error = MITTO_PAGE_ERROR_SCRIPT;
        event.message = std::move(evaluated.text);
      } else {
        event.json = std::move(evaluated.text);
      }
      Push(std::move(event));
      return;
    }
    case PageMessage::kConsole: {
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_CONSOLE;
      event.level = static_cast<int32_t>(frame->console.level);
      event.message = std::move(frame->console.text);
      PushReport(std::move(event));
      return;
    }
    case PageMessage::kUncaughtError: {
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_UNCAUGHT_ERROR;
      event.error = MITTO_PAGE_ERROR_SCRIPT;
      event.message = std::move(frame->uncaught_error);
      PushReport(std::move(event));
      return;
    }
    case PageMessage::kRequest:
      OnRequest(std::move(frame->request), frame->document);
      return;
    case PageMessage::kCredit:
      OnCredit(frame->credit);
      return;
    case PageMessage::kGetCookies:
      OnGetCookies(frame->cookies_question, CookieContextOf(frame->document));
      return;
    case PageMessage::kSetCookie:
      OnSetCookie(frame->cookie_line, CookieContextOf(frame->document));
      return;
    case PageMessage::kLoaded: {
      // The Page has loaded once its top-level window has, whatever its
      // frames do.
      if (context->parent != kNoDocument) {
        return;
      }
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_LOADED;
      Push(std::move(event));
      return;
    }
    case PageMessage::kCancel:
      OnCancel(frame->cancelled);
      return;
    case PageMessage::kRequestBodyPart:
      OnRequestBodyPart(frame->request_body_part);
      return;
    case PageMessage::kRequestBodyEnded:
      OnRequestBodyEnded(frame->request_body_ended);
      return;
    case PageMessage::kKnownGap: {
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_KNOWN_GAP;
      event.message = std::move(frame->known_gap);
      PushReport(std::move(event));
      return;
    }
    case PageMessage::kCookieStoreGet:
      OnCookieStoreGet(std::move(frame->cookie_store_get),
                       CookieContextOf(frame->document));
      return;
    case PageMessage::kCookieStoreSet:
      OnCookieStoreSet(std::move(frame->cookie_store_set),
                       CookieContextOf(frame->document));
      return;
    case PageMessage::kCookieStoreListen:
      OnCookieStoreListen(frame->cookie_store_listen, frame->document,
                          CookieContextOf(frame->document));
      return;
    case PageMessage::kBroadcast:
      OnBroadcast(frame->broadcast, frame->document);
      return;
    case PageMessage::kDocumentCommitted:
      OnDocumentCommitted(frame->committed);
      return;
    case PageMessage::kCacheStorage:
      OnCacheStorage(std::move(frame->cache_storage_call), frame->document,
                     document_url);
      return;
    case PageMessage::kSameDocumentNavigation:
      OnSameDocumentNavigation(frame->document, frame->same_document_url);
      return;
    case PageMessage::kAboutBlankNavigation:
      OnAboutBlankNavigation(frame->document, frame->about_blank_initiator,
                             frame->local_navigation_kind);
      return;
    case PageMessage::kDocumentOpened:
      OnDocumentOpened(frame->document, frame->opener);
      return;
    case PageMessage::kTraversalRequest:
      OnTraversalRequest(frame->traversal, frame->document);
      return;
    case PageMessage::kRecordsReleased:
      OnRecordsReleased(frame->released_records);
      return;
    case PageMessage::kIndexedDb:
      OnIndexedDb(std::move(frame->indexed_db_call), frame->document,
                  document_url);
      return;
    case PageMessage::kStorageBuckets:
      OnStorageBuckets(std::move(frame->storage_bucket_call), frame->document,
                       document_url);
      return;
    case PageMessage::kWorkerStarted:
      OnWorkerStarted(frame->worker_started);
      return;
    case PageMessage::kContextGone:
      OnContextGone(frame->gone_context);
      return;
    case PageMessage::kStorageBind:
      OnStorageBind(frame->storage_bind, frame->document);
      return;
    case PageMessage::kStorageGetAll:
      OnStorageGetAll(frame->storage_get_all);
      return;
    case PageMessage::kStoragePut:
    case PageMessage::kStorageDelete:
    case PageMessage::kStorageDeleteAll:
      OnStorageMutation(frame->type, std::move(frame->storage_mutation));
      return;
    case PageMessage::kStorageUnbind:
      OnStorageUnbind(frame->storage_unbind);
      return;
    case PageMessage::kNetworkHint:
      OnNetworkHint(frame->network_hint, frame->document);
      return;
    case PageMessage::kStart:
    case PageMessage::kEvaluate:
    case PageMessage::kResponseStarted:
    case PageMessage::kResponseBody:
    case PageMessage::kResponseEnded:
    case PageMessage::kCookies:
    case PageMessage::kCookieStoreCookies:
    case PageMessage::kCookieStoreStored:
    case PageMessage::kCookieStoreChange:
    case PageMessage::kBroadcastReceived:
    case PageMessage::kStorageEntries:
    case PageMessage::kStorageKeyChanged:
    case PageMessage::kStorageKeyChangeFailed:
    case PageMessage::kStorageKeyDeleted:
    case PageMessage::kStorageAllDeleted:
    case PageMessage::kStoragePart:
    case PageMessage::kCacheStorageAnswer:
    case PageMessage::kIndexedDbCallback:
    case PageMessage::kStorageBucketsAnswer:
    case PageMessage::kRequestBodyCredit:
    case PageMessage::kIsolation:
      // ReadFrameFromPage() refuses these.
      NOTREACHED();
  }
}

void PageHost::OnChannelClosed(PageChannel::Closed closed, std::string why) {
  if (closed == PageChannel::Closed::kFrameTooLarge) {
    Refuse(std::move(why));
    return;
  }
  WaitForExit();
}

void PageHost::Refuse(std::string why) {
  if (channel_) {
    channel_->Send(EncodeFrame(PageMessage::kRefused, why));
  }
  refused_by_library_ = std::move(why);
  process_->Kill();
  WaitForExit();
}

void PageHost::WaitForExit() {
  // A Page that has gone needs nothing more from the network or its storage:
  // a put that waits for the rest of its body from it fails, and its
  // databases' connections close, as a renderer's that goes. Not at once for
  // the Cache API and IndexedDB: what refuses the Page may be their own call.
  AbandonRequests();
  ReleaseStorage();
  if (indexed_db_) {
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(indexed_db_));
  }
  if (cache_storage_) {
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(cache_storage_));
  }
  // What was sent already still reaches the Page, if it reads it.
  channel_.reset();
  if (waiting_for_exit_ || ended_) {
    return;
  }
  waiting_for_exit_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(&PageProcess::WaitForExit, process_, kPageExitGrace),
      base::BindOnce(&PageHost::OnExited, weak_factory_.GetWeakPtr()));
}

void PageHost::OnExited(std::string how) {
  if (ended_) {
    return;
  }
  if (refused_by_library_) {
    End(MITTO_PAGE_ERROR_REFUSED,
        base::StrCat({"the library refused a message from the Page: ",
                      *refused_by_library_}));
  } else if (refused_by_page_) {
    End(MITTO_PAGE_ERROR_REFUSED,
        base::StrCat({"the Page refused a message from the library: ",
                      *refused_by_page_}));
  } else {
    End(MITTO_PAGE_ERROR_PROCESS_ENDED,
        base::StrCat({"the Page process ", how}));
  }
}

void PageHost::End(int32_t error, std::string message) {
  ended_ = true;
  for (const uint64_t call : calls_) {
    PageEvent event;
    event.type = MITTO_PAGE_EVENT_EVALUATED;
    event.error = error;
    event.call = call;
    event.message = message;
    Push(std::move(event));
  }
  calls_.clear();
  events_->PushEnded(error, std::move(message));
  ready_->Add(handle_, MITTO_READY_PAGE);
}

void PageHost::Push(PageEvent event) {
  events_->Push(std::move(event));
  ready_->Add(handle_, MITTO_READY_PAGE);
}

void PageHost::PushReport(PageEvent event) {
  // The list names a Page once for each of its events.
  for (int pushed = events_->PushReport(std::move(event)); pushed > 0;
       --pushed) {
    ready_->Add(handle_, MITTO_READY_PAGE);
  }
}

}  // namespace mitto
