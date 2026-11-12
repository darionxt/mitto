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
#include "mitto/ready_requests.h"
#include "net/base/io_buffer.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/base/schemeful_site.h"
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
         event.type == MITTO_PAGE_EVENT_UNCAUGHT_ERROR;
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

// One request of a Page's, which hears how its job goes and sends that to the
// Page, reading the body only as far as the Page has given credit.
class PageHost::Request final : public RequestClient {
 public:
  Request(PageHost& host, uint64_t number, GURL url, FetchChecks checks)
      : host_(host),
        number_(number),
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
    // The job follows each redirect, and the Page sees only where it ends.
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
  void OnFinished(int net_error) override {
    // The job goes after this, and so does this Request.
    job_ = nullptr;
    host_->OnRequestFinished(*this, net_error);
  }

 private:
  const raw_ref<PageHost> host_;
  const uint64_t number_;
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
    StartRequest(kNavigationRequest, *options_.url,
                 options_.navigation->headers, std::move(navigation_options),
                 std::nullopt, FetchChecks());
  }
  return true;
}

void PageHost::StartRequest(uint64_t number,
                            GURL url,
                            net::HttpRequestHeaders headers,
                            RequestOptions options,
                            std::optional<std::vector<char>> body,
                            FetchChecks checks) {
  auto request = std::make_unique<Request>(*this, number, url, checks);
  Request& client = *request;
  requests_.emplace(number, std::move(request));
  client.set_job(
      network_->StartPageRequest(client, std::move(url), std::move(headers),
                                 std::move(options), std::move(body)));
}

void PageHost::OnRequest(PageRequest asked) {
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
  } else if (!document_url_.is_valid()) {
    // A Page with no document from the Session has no Initiator that a
    // request can name.
    net_error = net::ERR_ACCESS_DENIED;
  } else if (is_fetch &&
             !url::Origin::Create(document_url_).IsSameOriginWith(url)) {
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
  options.fetch->initiator = document_url_;
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
  StartRequest(asked.number, std::move(url), std::move(headers),
               std::move(options), std::move(body),
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
    document_url_ = GURL(event.url);
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
      response.navigation_start =
          (request.start() - base::TimeTicks()).InMicroseconds();
      response.connection = ConnectionOf(document_url_);
    }
    response.url = event.url;
    response.status_text = event.status_text;
    response.headers = HeadersForPage(event.headers);
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
  const url::Origin document_origin = url::Origin::Create(document_url_);
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
    channel_->Send(EncodeResponseEnded(ended));
  }
  requests_.erase(number);
}

void PageHost::AbandonRequests() {
  // Each Request abandons its job as it goes.
  requests_.clear();
}

bool PageHost::DocumentHasCookies() const {
  return document_url_.is_valid() && document_url_.SchemeIsHTTPOrHTTPS();
}

std::optional<net::CookiePartitionKey> PageHost::DocumentPartitionKey() const {
  // RenderFrameHostImpl::GetIsolationInfoForSubresources() for a top-level
  // frame, which its RestrictedCookieManager gets.
  // Upstream: content/browser/renderer_host/render_frame_host_impl.cc
  const url::Origin origin = url::Origin::Create(document_url_);
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
void PageHost::OnGetCookies(uint64_t question) {
  if (!DocumentHasCookies()) {
    channel_->Send(EncodeCookies(question, ""));
    return;
  }
  const net::SiteForCookies site_for_cookies =
      net::SiteForCookies::FromUrl(document_url_);
  net::CookieOptions options;
  options.set_exclude_httponly();
  options.set_same_site_cookie_context(
      net::cookie_util::ComputeSameSiteContextForScriptGet(
          document_url_, site_for_cookies, /*initiator=*/std::nullopt,
          /*force_ignore_site_for_cookies=*/false));
  network_->cookie_store().GetCookieListWithOptionsAsync(
      document_url_, options,
      net::CookiePartitionKeyCollection(DocumentPartitionKey()),
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

void PageHost::OnSetCookie(const std::string& line) {
  if (!DocumentHasCookies()) {
    return;
  }
  const std::optional<net::CookiePartitionKey> partition_key =
      DocumentPartitionKey();
  net::CookieInclusionStatus status;
  std::unique_ptr<net::CanonicalCookie> cookie = net::CanonicalCookie::Create(
      document_url_, line, base::Time::Now(), /*server_time=*/std::nullopt,
      partition_key, net::CookieSourceType::kScript, &status);
  if (!cookie) {
    return;
  }
  const net::SiteForCookies site_for_cookies =
      net::SiteForCookies::FromUrl(document_url_);
  net::CookieOptions options;
  options.set_exclude_httponly();
  options.set_same_site_cookie_context(
      net::cookie_util::ComputeSameSiteContextForScriptSet(
          document_url_, site_for_cookies,
          /*force_ignore_site_for_cookies=*/false));
  network_->cookie_store().SetCanonicalCookieAsync(
      std::move(cookie), document_url_, options, base::DoNothing(),
      /*cookie_access_result=*/std::nullopt);
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
      OnRequest(std::move(frame->request));
      return;
    case PageMessage::kCredit:
      OnCredit(frame->credit);
      return;
    case PageMessage::kGetCookies:
      OnGetCookies(frame->cookies_question);
      return;
    case PageMessage::kSetCookie:
      OnSetCookie(frame->cookie_line);
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
    case PageMessage::kStart:
    case PageMessage::kEvaluate:
    case PageMessage::kResponseStarted:
    case PageMessage::kResponseBody:
    case PageMessage::kResponseEnded:
    case PageMessage::kCookies:
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
