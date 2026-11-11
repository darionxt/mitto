#include "mitto/page_host.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

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

namespace mitto {
namespace {

constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kTimeZoneKey = "time_zone";
constexpr std::string_view kLocaleKey = "locale";
constexpr std::string_view kUrlKey = "url";
constexpr std::string_view kRequestsKey = "requests";
constexpr std::string_view kNavigationKey = "navigation";
constexpr std::string_view kScriptKey = "script";
constexpr std::string_view kFetchKey = "fetch";
constexpr std::string_view kHeadersKey = "headers";
constexpr std::string_view kOptionsKey = "options";

// What a Page reports when the caller names nothing.
constexpr std::string_view kDefaultTimeZone = "UTC";
constexpr std::string_view kDefaultLocale = "en-US";

// Names of time zones and tags of locales are short and ASCII. Which ones exist
// only the Page knows, because only it holds ICU's whole data.
constexpr size_t kMaxNameSize = 64;

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

bool IsName(std::string_view name) {
  return !name.empty() && name.size() <= kMaxNameSize &&
         std::ranges::all_of(name, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-' ||
                  c == '+' || c == '/';
         });
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
        key == kNavigationKey ? &options.navigation
        : key == kScriptKey   ? &options.script
        : key == kFetchKey    ? &options.fetch
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
  options.start.time_zone = kDefaultTimeZone;
  options.start.locale = kDefaultLocale;
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
    std::string* const name = key == kTimeZoneKey ? &options.start.time_zone
                              : key == kLocaleKey ? &options.start.locale
                                                  : nullptr;
    if (!name || !value.is_string() || !IsName(value.GetString())) {
      return std::nullopt;
    }
    *name = value.GetString();
  }
  // A navigation sends what the Persona's says.
  if (options.url && !options.navigation) {
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
  Request(PageHost& host, uint64_t number)
      : host_(host),
        number_(number),
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
  // The Page reads these in order, so nothing waits for its hello.
  channel_->Send(EncodeHello());
  channel_->Send(EncodeStart(options_.start));
  if (options_.url) {
    // What session.get(url) sends: a navigation that the browser started, from
    // no page.
    StartRequest(kNavigationRequest, *options_.url, *options_.navigation,
                 std::nullopt);
  }
  return true;
}

void PageHost::StartRequest(uint64_t number,
                            GURL url,
                            const PageRequestTemplate& request_template,
                            std::optional<GURL> initiator) {
  RequestOptions request_options = request_template.options;
  request_options.fetch->initiator = std::move(initiator);
  auto request = std::make_unique<Request>(*this, number);
  Request& client = *request;
  requests_.emplace(number, std::move(request));
  client.set_job(network_->StartPageRequest(client, std::move(url),
                                            request_template.headers,
                                            std::move(request_options)));
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
  const std::optional<PageRequestTemplate>& request_template =
      asked.kind == PageRequestKind::kScript ? options_.script : options_.fetch;
  GURL url(asked.url);
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
  }
  if (net_error != net::OK) {
    PageResponseEnded ended;
    ended.number = asked.number;
    ended.net_error = net_error;
    channel_->Send(EncodeResponseEnded(ended));
    return;
  }
  StartRequest(asked.number, std::move(url), *request_template, document_url_);
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
    response.url = event.url;
    response.headers = HeadersForPage(event.headers);
    channel_->Send(EncodeResponseStarted(response));
  }
  request.ReadIfCredited();
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
