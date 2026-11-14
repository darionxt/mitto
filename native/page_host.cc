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
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "mitto/header_rules.h"
#include "mitto/mitto.h"
#include "mitto/page_cache_storage.h"
#include "mitto/ready_requests.h"
#include "net/base/io_buffer.h"
#include "net/base/isolation_info.h"
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
#include "net/http/http_util.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

constexpr std::string_view kVersionKey = "version";
// What net::HttpConnectionInfoToString() and net::NextProtoToString() call a
// protocol that is not known.
constexpr char kUnknownProtocol[] = "unknown";
constexpr std::string_view kStartKey = "start";
constexpr std::string_view kUrlKey = "url";
constexpr std::string_view kRequestsKey = "requests";
constexpr std::string_view kNavigationKey = "navigation";
constexpr std::string_view kScriptKey = "script";
constexpr std::string_view kFetchKey = "fetch";
constexpr std::string_view kFetchWithBodyKey = "fetch_with_body";
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
  for (const auto [key, kind] : value.GetDict()) {
    std::optional<PageRequestTemplate>* const slot =
        key == kNavigationKey      ? &options.navigation
        : key == kScriptKey        ? &options.script
        : key == kFetchKey         ? &options.fetch
        : key == kFetchWithBodyKey ? &options.fetch_with_body
                                   : nullptr;
    if (!slot) {
      return false;
    }
    *slot = ReadTemplate(kind);
    if (!*slot) {
      return false;
    }
  }
  return true;
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
  if (!has_start || (options.url && !options.navigation)) {
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
  const size_t bytes =
      event.message.size() + (note ? note->message.size() : 0);
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
        document_origin_(std::move(document_origin)),
        last_url_(std::move(url)),
        checks_(checks),
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
  bool is_navigation() const { return number_ == kNavigationRequest; }
  // The origin of the document that asked, opaque for a navigation.
  const url::Origin& document_origin() const { return document_origin_; }
  const FetchChecks& checks() const { return checks_; }
  bool cors() const { return cors_; }
  bool tainted() const { return tainted_; }

  // Follows the request to its response at `url`, a redirect's or the last,
  // and works out its response tainting and its tainted origin as the Fetch
  // Standard does, for a Page's document at `document_origin`.
  void Reach(const GURL& url, const url::Origin& document_origin) {
    // A redirect from a URL of another origin than the document's to one of
    // yet another origin taints the request's origin.
    if (!url::IsSameOriginWith(url, last_url_) &&
        !document_origin.IsSameOriginWith(last_url_)) {
      tainted_ = true;
    }
    // Once any hop goes to another origin, the response is a CORS one.
    if (!document_origin.IsSameOriginWith(url)) {
      cors_ = true;
    }
    last_url_ = url;
  }
  base::TimeTicks start() const { return start_; }
  bool started() const { return started_; }
  uint32_t credit() const { return credit_; }

  // What the Performance Timeline needs of the hops so far.
  bool timing_allow_passed() const { return !timing_allow_failed_; }
  // Whether the response's tainting is opaque: a script's, which is a fetch of
  // mode no-cors, once a hop has left the document's origin. A navigation's
  // is basic.
  bool opaque() const {
    return !is_navigation() && !checks_.is_fetch && timing_cross_origin_;
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
  }

  void set_job(base::WeakPtr<RunningRequest> job) { job_ = std::move(job); }
  void set_started() { started_ = true; }
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

  // RequestClient:
  bool AllowsResponse(const RequestEvent& event) override {
    return host_->AllowsResponse(*this, event);
  }
  void OnResponseEvent(RequestEvent event) override {
    CheckTimingAllowOrigin(event, document_origin_);
    // The job follows each redirect, and the Page sees only where it ends.
    if (event.type == MITTO_EVENT_REDIRECT_FOLLOWED) {
      FollowRedirect(event, document_origin_);
    }
    if (event.type == MITTO_EVENT_RESPONSE_STARTED) {
      host_->OnResponseStarted(*this, event);
    }
  }
  net::IOBuffer* read_buffer() override { return buffer_.get(); }
  int read_size() const override { return static_cast<int>(credit_); }
  void OnBodyRead(size_t size) override {
    reading_ = false;
    host_->OnBodyRead(*this, size);
  }
  void OnTotals(const RequestTotals& totals) override { totals_ = totals; }
  void OnFinished(int net_error) override {
    // The job goes after this, and so does this Request.
    job_ = nullptr;
    host_->OnRequestFinished(*this, net_error);
  }

 private:
  const raw_ref<PageHost> host_;
  const uint64_t number_;
  const url::Origin document_origin_;
  // The URL of the hop the request is at.
  GURL last_url_;
  const FetchChecks checks_;
  bool cors_ = false;
  bool tainted_ = false;
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
  base::TimeTicks last_redirect_end_;
  RequestTotals totals_;
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
  cache_storage_.reset();
  AbandonRequests();
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
    RequestOptions navigation_options = options_.navigation->options;
    navigation_options.fetch->initiator = std::nullopt;
    StartRequest(kNavigationRequest, url::Origin(), *options_.url,
                 options_.navigation->headers, std::move(navigation_options),
                 std::nullopt, FetchChecks());
  }
  return true;
}

void PageHost::StartRequest(uint64_t number,
                            const url::Origin& document_origin,
                            GURL url,
                            net::HttpRequestHeaders headers,
                            RequestOptions options,
                            std::optional<std::vector<char>> body,
                            FetchChecks checks) {
  auto request =
      std::make_unique<Request>(*this, number, document_origin, url, checks);
  Request& client = *request;
  requests_.emplace(number, std::move(request));
  client.set_job(
      network_->StartPageRequest(client, std::move(url), std::move(headers),
                                 std::move(options), std::move(body)));
}

void PageHost::OnRequest(PageRequest asked, const GURL& document_url) {
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
  const bool is_fetch = asked.kind == PageRequestKind::kFetch;
  const std::optional<PageRequestTemplate>& request_template =
      !is_fetch    ? options_.script
      : asked.body ? options_.fetch_with_body
                   : options_.fetch;
  GURL url(asked.url);
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
  bool unsafe_lines = false;
  for (const auto& [name, value] : lines) {
    unsafe_lines |= !IsCorsSafelistedRequestHeader(name, value);
  }
  // These fail as such a request fails in Chrome, and the Page goes on.
  int net_error = net::OK;
  if (!url.is_valid()) {
    net_error = net::ERR_INVALID_URL;
  } else if (!url.SchemeIsHTTPOrHTTPS()) {
    net_error = net::ERR_UNKNOWN_URL_SCHEME;
  } else if (!request_template) {
    net_error = net::ERR_NOT_IMPLEMENTED;
  } else if (url::Origin::Create(document_url).opaque()) {
    // A document that the Session did not load, such as about:blank, has no
    // Initiator that a request can name.
    net_error = net::ERR_ACCESS_DENIED;
  } else if (is_fetch &&
             !url::Origin::Create(document_url).IsSameOriginWith(url)) {
    // A cross-origin fetch, which the CORS check holds to its response. One
    // of mode same-origin fails, as in Chrome. One that would need a CORS
    // preflight first is a Known Gap: the preflight is a request that no
    // Persona records yet.
    if (asked.same_origin) {
      net_error = net::ERR_FAILED;
    } else if (!IsCorsSafelistedMethod(asked.method) || unsafe_lines) {
      net_error = net::ERR_NOT_IMPLEMENTED;
    }
  }
  if (net_error != net::OK) {
    PageResponseEnded ended;
    ended.number = asked.number;
    ended.net_error = net_error;
    channel_->Send(EncodeResponseEnded(ended));
    return;
  }
  // The script's lines take the places of the Persona's lines of their names,
  // and the rest follow, as a caller's headers do. A Content-Type that the
  // script does not give goes, as a caller's body without one does.
  net::HttpRequestHeaders headers = request_template->headers;
  RequestOptions options = request_template->options;
  options.method = asked.method;
  options.fetch->initiator = document_url;
  if (asked.body && !std::ranges::any_of(lines, [](const auto& entry) {
        return base::EqualsCaseInsensitiveASCII(
            entry.first, net::HttpRequestHeaders::kContentType);
      })) {
    headers.RemoveHeader(net::HttpRequestHeaders::kContentType);
  }
  for (const auto& [name, value] : lines) {
    headers.SetHeader(name, value);
    options.fetch->fixed_headers.insert(base::ToLowerASCII(name));
  }
  std::optional<std::vector<char>> body;
  if (asked.body) {
    body.emplace(asked.body->begin(), asked.body->end());
  }
  StartRequest(asked.number, url::Origin::Create(document_url), std::move(url),
               std::move(headers), std::move(options), std::move(body),
               FetchChecks{.is_fetch = is_fetch,
                           .unsafe_lines = unsafe_lines,
                           .same_origin = asked.same_origin});
}

void PageHost::OnCancel(uint64_t number) {
  const auto found = requests_.find(number);
  if (found == requests_.end()) {
    // The request may have ended since the Page let it go.
    if (number > last_request_number_) {
      Refuse(base::StrCat({"a cancel of request ", base::NumberToString(number),
                           ", which the Page has not asked for"}));
    }
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

void PageHost::OnResponseStarted(Request& request, const RequestEvent& event) {
  request.set_started();
  if (request.is_navigation()) {
    // The document that the Page makes of it, which it tells the library of.
    navigation_responses_[request.number()] = GURL(event.url);
    PageEvent navigated;
    navigated.type = MITTO_PAGE_EVENT_NAVIGATED;
    navigated.status = event.status;
    navigated.url = event.url;
    Push(std::move(navigated));
  }
  if (channel_) {
    PageResponseStarted response;
    response.number = request.number();
    response.status = event.status;
    response.redirected = event.redirected;
    response.cors = request.cors();
    if (request.is_navigation()) {
      response.navigation_start = Microseconds(
          options_.navigation_start.is_null() ? request.start()
                                              : options_.navigation_start);
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

bool PageHost::AllowsResponse(Request& request, const RequestEvent& event) {
  const FetchChecks& checks = request.checks();
  if (!checks.is_fetch) {
    return true;
  }
  // Upstream: services/network/cors/cors_url_loader.cc
  // Upstream: services/network/public/cpp/cors/cors.cc
  // The Fetch Standard's CORS check, on each response of a fetch whose
  // response tainting is cors, redirects included, before the redirect is
  // followed or any of the response goes to the Page, as Chrome's network
  // service does it. A Page's cross-origin fetches have no credentials, so
  // Access-Control-Allow-Origin has to name the request's origin, which a
  // tainted one serializes as "null", or be "*".
  const url::Origin& document_origin = request.document_origin();
  request.Reach(GURL(event.url), document_origin);
  // A redirect to another origin of a request that would need a CORS
  // preflight there is not followed: the preflight is a Known Gap, and
  // following without it would send what the script asked for to a server
  // that has not agreed to it.
  // Nor is one of mode same-origin, as in Chrome.
  if (event.type == MITTO_EVENT_REDIRECT_FOLLOWED &&
      !document_origin.IsSameOriginWith(GURL(event.new_url)) &&
      (checks.same_origin || !IsCorsSafelistedMethod(event.new_method) ||
       checks.unsafe_lines)) {
    return false;
  }
  if (!request.cors()) {
    return true;
  }
  const std::string origin =
      request.tainted() ? std::string("null") : document_origin.Serialize();
  std::optional<std::string> allowed;
  for (const std::string_view line : base::SplitStringPieceUsingSubstr(
           event.headers, "\r\n", base::KEEP_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos ||
        !base::EqualsCaseInsensitiveASCII(line.substr(0, colon),
                                          "access-control-allow-origin")) {
      continue;
    }
    // More than one line is a failure, as a value that lists more than one.
    if (allowed) {
      return false;
    }
    allowed = std::string(
        base::TrimWhitespaceASCII(line.substr(colon + 1), base::TRIM_ALL));
  }
  return allowed && (*allowed == "*" || *allowed == origin);
}

void PageHost::OnBodyRead(Request& request, size_t size) {
  const base::span<const uint8_t> bytes = request.TakeRead(size);
  if (channel_) {
    channel_->Send(
        EncodeResponseBody(request.number(), base::as_string_view(bytes)));
  }
  request.ReadIfCredited();
}

void PageHost::OnRequestFinished(Request& request, int net_error) {
  const uint64_t number = request.number();
  if (request.is_navigation() && !request.started()) {
    // The Page keeps its document, and hears nothing of this.
    PageEvent navigated;
    navigated.type = MITTO_PAGE_EVENT_NAVIGATED;
    navigated.net_error = net_error == net::OK ? net::ERR_FAILED : net_error;
    navigated.url = options_.url->possibly_invalid_spec();
    Push(std::move(navigated));
  } else if (channel_) {
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
  // Each Request abandons its job as it goes.
  requests_.clear();
}

// static
bool PageHost::DocumentHasCookies(const GURL& document_url) {
  return document_url.is_valid() && document_url.SchemeIsHTTPOrHTTPS();
}

// static
std::optional<net::CookiePartitionKey> PageHost::DocumentPartitionKey(
    const GURL& document_url) {
  // RenderFrameHostImpl::GetIsolationInfoForSubresources() for a top-level
  // frame, which its RestrictedCookieManager gets.
  // Upstream: content/browser/renderer_host/render_frame_host_impl.cc
  const url::Origin origin = url::Origin::Create(document_url);
  const net::IsolationInfo isolation_info = net::IsolationInfo::Create(
      net::IsolationInfo::RequestType::kOther, origin, origin,
      net::SiteForCookies::FromOrigin(origin));
  return net::CookiePartitionKey::FromNetworkIsolationKey(
      isolation_info.network_isolation_key(), isolation_info.site_for_cookies(),
      net::SchemefulSite(origin), isolation_info.IsMainFrameRequest());
}

// What RestrictedCookieManager does for document.cookie, with the SCRIPT role
// and always for the document that the Session loaded, never for a URL that
// the Page names.
// Upstream: services/network/restricted_cookie_manager.cc
void PageHost::OnGetCookies(uint64_t question, const GURL& document_url) {
  if (!DocumentHasCookies(document_url)) {
    channel_->Send(EncodeCookies(question, ""));
    return;
  }
  const net::SiteForCookies site_for_cookies =
      net::SiteForCookies::FromUrl(document_url);
  net::CookieOptions options;
  options.set_exclude_httponly();
  options.set_same_site_cookie_context(
      net::cookie_util::ComputeSameSiteContextForScriptGet(
          document_url, site_for_cookies, /*initiator=*/std::nullopt,
          /*force_ignore_site_for_cookies=*/false));
  network_->cookie_store().GetCookieListWithOptionsAsync(
      document_url, options,
      net::CookiePartitionKeyCollection(DocumentPartitionKey(document_url)),
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

void PageHost::OnSetCookie(const std::string& line, const GURL& document_url) {
  if (!DocumentHasCookies(document_url)) {
    return;
  }
  const std::optional<net::CookiePartitionKey> partition_key =
      DocumentPartitionKey(document_url);
  net::CookieInclusionStatus status;
  std::unique_ptr<net::CanonicalCookie> cookie = net::CanonicalCookie::Create(
      document_url, line, base::Time::Now(), /*server_time=*/std::nullopt,
      partition_key, net::CookieSourceType::kScript, &status);
  if (!cookie) {
    return;
  }
  const net::SiteForCookies site_for_cookies =
      net::SiteForCookies::FromUrl(document_url);
  net::CookieOptions options;
  options.set_exclude_httponly();
  options.set_same_site_cookie_context(
      net::cookie_util::ComputeSameSiteContextForScriptSet(
          document_url, site_for_cookies,
          /*force_ignore_site_for_cookies=*/false));
  network_->cookie_store().SetCanonicalCookieAsync(
      std::move(cookie), document_url, options, base::DoNothing(),
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
net::CookieOptions ScriptOptions(const GURL& url, bool set) {
  const net::SiteForCookies site_for_cookies =
      net::SiteForCookies::FromUrl(url);
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
                                const GURL& document_url) {
  if (!DocumentHasCookies(document_url)) {
    SendCookieStoreCookies(get, {}, {});
    return;
  }
  network_->cookie_store().GetCookieListWithOptionsAsync(
      document_url, ScriptOptions(document_url, /*set=*/false),
      net::CookiePartitionKeyCollection(DocumentPartitionKey(document_url)),
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
                                const GURL& document_url) {
  const uint64_t question = set.question;
  const auto answer = [this, question](bool stored) {
    if (channel_) {
      channel_->Send(EncodeCookieStoreStored(question, stored));
    }
  };
  if (!DocumentHasCookies(document_url)) {
    answer(false);
    return;
  }
  const std::optional<net::CookiePartitionKey> partition_key =
      set.partitioned ? DocumentPartitionKey(document_url) : std::nullopt;
  const base::Time expiry =
      base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(set.expiry));
  net::CookieInclusionStatus status;
  std::unique_ptr<net::CanonicalCookie> cookie =
      net::CanonicalCookie::CreateSanitizedCookie(
          document_url, set.name, set.value, set.domain, set.path,
          /*creation_time=*/base::Time(), expiry,
          /*last_access_time=*/base::Time(), /*secure=*/true,
          /*http_only=*/false, ToNetSameSite(set.same_site),
          net::COOKIE_PRIORITY_MEDIUM, partition_key, &status);
  // IsCookieDomainValid(): the Page checks the domain first, so one that
  // fails here comes from a Page that does not.
  if ((cookie && !cookie->IsDomainMatch(document_url.host())) ||
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
  if (!net::cookie_util::DomainIsHostOnly(document_url.host())) {
    status.AddExclusionReason(
        net::CookieInclusionStatus::ExclusionReason::EXCLUDE_INVALID_DOMAIN);
  }
  if (!status.IsInclude()) {
    answer(false);
    return;
  }
  const url::Origin origin = url::Origin::Create(document_url);
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
      ScriptOptions(document_url, /*set=*/true),
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
void PageHost::OnCookieStoreListen(bool listen, const GURL& document_url) {
  if (!listen) {
    cookie_store_subscription_.reset();
    return;
  }
  if (cookie_store_subscription_ || !DocumentHasCookies(document_url)) {
    return;
  }
  cookie_store_url_ = document_url;
  // The dispatcher stops calling once the subscription goes, which this
  // owns.
  cookie_store_subscription_ =
      network_->cookie_store().GetChangeDispatcher().AddCallbackForUrl(
          document_url, DocumentPartitionKey(document_url),
          base::BindRepeating(&PageHost::OnCookieStoreChange,
                              base::Unretained(this)));
}

// RestrictedCookieManager::Listener::OnCookieChange() and
// CookieChangeEvent::ToEventInfo(): a change that the document's script may
// see, but for an overwrite, which the insertion after it stands for.
void PageHost::OnCookieStoreChange(const net::CookieChangeInfo& change) {
  if (!channel_) {
    return;
  }
  const net::CookieAccessDelegate* const delegate =
      network_->cookie_store().cookie_access_delegate();
  const bool trustworthy =
      delegate && delegate->ShouldTreatUrlAsTrustworthy(cookie_store_url_);
  if (!change.cookie
           .IncludeForRequestURL(
               cookie_store_url_,
               ScriptOptions(cookie_store_url_, /*set=*/false),
               net::CookieAccessParams(change.access_result.access_semantics,
                                       change.access_result.scope_semantics,
                                       trustworthy))
           .status.IsInclude()) {
    return;
  }
  PageCookieStoreChange page_change;
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
                           const GURL& document_url) {
  // Only a document of an origin that is not opaque sends one: one of an
  // opaque origin shares its channels with no other.
  const url::Origin origin = url::Origin::Create(document_url);
  if (!DocumentHasCookies(document_url) || origin.opaque()) {
    Refuse(
        "a broadcast from a Page with no document of an origin that "
        "another can share");
    return;
  }
  network_->BroadcastFromPage(*this, origin, broadcast);
}

void PageHost::OnCacheStorage(PageCacheStorageCall call,
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
            weak_factory_.GetWeakPtr()));
  }
  cache_storage_->OnCall(std::move(call),
                         DocumentCacheStorageKey(document_url));
}

// static
std::optional<std::string> PageHost::DocumentCacheStorageKey(
    const GURL& document_url) {
  // GlobalCacheStorage::CanCreateCacheStorage() and caches' [SecureContext]:
  // an origin that is not opaque, of a potentially trustworthy URL.
  if (!DocumentHasCookies(document_url)) {
    return std::nullopt;
  }
  if (!document_url.SchemeIs(url::kHttpsScheme) &&
      !net::IsLocalhost(document_url)) {
    return std::nullopt;
  }
  // StorageKey::Serialize() of a first-party key: the origin and a slash.
  return url::Origin::Create(document_url).GetURL().spec();
}

void PageHost::DeliverBroadcast(const url::Origin& origin,
                                const PageBroadcast& broadcast) {
  const GURL document_url = TopDocumentUrl();
  if (!channel_ || ended_ || refused_by_library_ ||
      !DocumentHasCookies(document_url) ||
      !url::Origin::Create(document_url).IsSameOriginWith(origin)) {
    return;
  }
  channel_->Send(EncodeBroadcastReceived(broadcast));
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
  channel_.reset();
  End(MITTO_PAGE_ERROR_SESSION_DESTROYED, "the Session was destroyed");
}

void PageHost::OnDocumentCommitted(const PageDocumentCommitted& committed) {
  if (committed.document <= last_document_id_) {
    Refuse(base::StrCat(
        {"a document numbered ", base::NumberToString(committed.document),
         ", after one numbered ", base::NumberToString(last_document_id_)}));
    return;
  }
  GURL url;
  switch (committed.source) {
    case PageDocumentSource::kInitialEmpty:
      // The Page's first document, before any other.
      if (last_document_id_ != kNoDocument) {
        Refuse("an initial about:blank after another document");
        return;
      }
      url = GURL(url::kAboutBlankURL);
      break;
    case PageDocumentSource::kNavigationResponse: {
      const auto found = navigation_responses_.find(committed.navigation);
      if (found == navigation_responses_.end()) {
        Refuse(base::StrCat({"a document of navigation ",
                             base::NumberToString(committed.navigation),
                             ", which has no response"}));
        return;
      }
      url = std::move(found->second);
      navigation_responses_.erase(found);
      break;
    }
  }
  last_document_id_ = committed.document;
  // It takes the place of the top-level document, the only one a Page has,
  // whose messages all came before this.
  documents_.clear();
  documents_.emplace(committed.document, std::move(url));
  // The old document's CookieStore listened over a pipe that went with it.
  cookie_store_subscription_.reset();
}

const GURL* PageHost::DocumentUrl(uint64_t document, uint32_t type) {
  const auto found = documents_.find(document);
  if (found == documents_.end()) {
    Refuse(base::StrCat({"message type ", base::NumberToString(type),
                         " of document ", base::NumberToString(document),
                         ", which is not one of the Page's"}));
    return nullptr;
  }
  return &found->second;
}

GURL PageHost::TopDocumentUrl() const {
  return documents_.empty() ? GURL() : documents_.begin()->second;
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
  // What a message of a document asks for is the document's, as the library
  // loaded it.
  GURL document_url;
  if (NamesDocument(frame->type)) {
    const GURL* const url = DocumentUrl(frame->document, type);
    if (!url) {
      return;
    }
    document_url = *url;
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
      OnRequest(std::move(frame->request), document_url);
      return;
    case PageMessage::kCredit:
      OnCredit(frame->credit);
      return;
    case PageMessage::kGetCookies:
      OnGetCookies(frame->cookies_question, document_url);
      return;
    case PageMessage::kSetCookie:
      OnSetCookie(frame->cookie_line, document_url);
      return;
    case PageMessage::kLoaded: {
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_LOADED;
      Push(std::move(event));
      return;
    }
    case PageMessage::kCancel:
      OnCancel(frame->cancelled);
      return;
    case PageMessage::kKnownGap: {
      PageEvent event;
      event.type = MITTO_PAGE_EVENT_KNOWN_GAP;
      event.message = std::move(frame->known_gap);
      PushReport(std::move(event));
      return;
    }
    case PageMessage::kCookieStoreGet:
      OnCookieStoreGet(std::move(frame->cookie_store_get), document_url);
      return;
    case PageMessage::kCookieStoreSet:
      OnCookieStoreSet(std::move(frame->cookie_store_set), document_url);
      return;
    case PageMessage::kCookieStoreListen:
      OnCookieStoreListen(frame->cookie_store_listen, document_url);
      return;
    case PageMessage::kBroadcast:
      OnBroadcast(frame->broadcast, document_url);
      return;
    case PageMessage::kDocumentCommitted:
      OnDocumentCommitted(frame->committed);
      return;
    case PageMessage::kCacheStorage:
      OnCacheStorage(std::move(frame->cache_storage_call), document_url);
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
    case PageMessage::kCacheStorageAnswer:
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
  // A Page that has gone needs nothing more from the network.
  AbandonRequests();
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
