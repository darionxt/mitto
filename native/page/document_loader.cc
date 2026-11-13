#include "mitto/page/document_loader.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/event.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/performance_timing_confidence.h"
#include "mitto/dom/window.h"
#include "mitto/page/resource_timing.h"
#include "mitto/page/script_runner.h"

namespace mitto {
namespace {

// The HTML Standard's JavaScript MIME types, which Blink's
// MIMETypeRegistry::IsSupportedJavaScriptMIMEType() holds.
// Upstream: third_party/blink/renderer/platform/network/mime/mime_type_registry.cc
constexpr std::array<std::u16string_view, 16> kJavaScriptMimeTypes = {
    u"application/ecmascript",
    u"application/javascript",
    u"application/x-ecmascript",
    u"application/x-javascript",
    u"text/ecmascript",
    u"text/javascript",
    u"text/javascript1.0",
    u"text/javascript1.1",
    u"text/javascript1.2",
    u"text/javascript1.3",
    u"text/javascript1.4",
    u"text/javascript1.5",
    u"text/jscript",
    u"text/livescript",
    u"text/x-ecmascript",
    u"text/x-javascript",
};

bool IsJavaScriptMimeType(std::u16string_view mime_type) {
  for (const std::u16string_view javascript : kJavaScriptMimeTypes) {
    if (mime_type == javascript) {
      return true;
    }
  }
  return false;
}

enum class ScriptType {
  kClassic,
  kModule,
  // Not a script that runs, such as a data block or an import map.
  kOther,
};

// The HTML Standard's script block's type string, from the type and language
// attributes, as Blink's ScriptLoader::GetScriptTypeAtPrepare() reads it.
// Upstream: third_party/blink/renderer/core/script/script_loader.cc
ScriptType TypeOf(const HTMLScriptElement& element) {
  const std::optional<std::u16string> type = element.GetAttribute(u"type");
  const std::optional<std::u16string> language =
      element.GetAttribute(u"language");
  if ((type && type->empty()) || (!type && (!language || language->empty()))) {
    return ScriptType::kClassic;
  }
  const std::u16string block_type =
      type ? std::u16string(base::TrimWhitespace(*type, base::TRIM_ALL))
           : u"text/" + *language;
  const std::u16string lower = base::ToLowerASCII(block_type);
  if (IsJavaScriptMimeType(lower)) {
    return ScriptType::kClassic;
  }
  return lower == u"module" ? ScriptType::kModule : ScriptType::kOther;
}

// Blink's ParseContentTypeOptionsHeader(), as the Fetch Standard splits the
// header.
bool IsNosniff(std::string_view headers) {
  const std::string field = HeaderField(headers, "x-content-type-options");
  const std::string_view first = std::string_view(field).substr(
      0, std::min(field.find(','), field.size()));
  return base::EqualsCaseInsensitiveASCII(
      base::TrimString(first, " \t", base::TRIM_ALL), "nosniff");
}

// KURL::ElidedString(), for a URL in a console message.
// Upstream: third_party/blink/renderer/platform/weborigin/kurl.cc
std::string ElidedUrl(const GURL& url) {
  const std::string& spec = url.possibly_invalid_spec();
  if (spec.size() <= 1024) {
    return spec;
  }
  return base::StrCat(
      {spec.substr(0, 511), "...", spec.substr(spec.size() - 510)});
}

// Blink's AllowedByNosniff::MimeTypeAsScript() for a classic script of an
// element: whether a response of `url` with `headers` may run as one, and
// otherwise the console error that says why not.
// Upstream: third_party/blink/renderer/platform/loader/allowed_by_nosniff.cc
std::optional<std::string> RefusedAsScript(const GURL& url,
                                           std::string_view headers) {
  const std::string mime_type = HttpContentType(headers);
  const bool is_javascript =
      IsJavaScriptMimeType(base::ASCIIToUTF16(mime_type));
  if (IsNosniff(headers) && !is_javascript) {
    return base::StrCat({"Refused to execute script from '", ElidedUrl(url),
                         "' because its MIME type ('", mime_type,
                         "') is not executable, and strict MIME type "
                         "checking is enabled."});
  }
  if (is_javascript) {
    return std::nullopt;
  }
  // The Fetch Standard's should response to request be blocked due to MIME
  // type.
  for (const std::string_view blocked : {"image/", "audio/", "video/"}) {
    if (mime_type.starts_with(blocked)) {
      return base::StrCat({"Refused to execute script from '", ElidedUrl(url),
                           "' because its MIME type ('", mime_type,
                           "') is not executable."});
    }
  }
  if (mime_type.starts_with("text/csv")) {
    return base::StrCat({"Refused to execute script from '", ElidedUrl(url),
                         "' because its MIME type ('", mime_type,
                         "') is not executable."});
  }
  return std::nullopt;
}

// HTMLScriptElement::IsPotentiallyRenderBlocking() for a classic script that
// the parser inserted: one with blocking="render", or without async and defer.
// Upstream: third_party/blink/renderer/core/html/html_script_element.cc
bool IsPotentiallyRenderBlocking(const HTMLScriptElement& element) {
  if (const std::optional<std::u16string> blocking =
          element.GetAttribute(u"blocking")) {
    for (const std::u16string_view token :
         base::SplitStringPiece(*blocking, u" \t\n\f\r", base::KEEP_WHITESPACE,
                                base::SPLIT_WANT_NONEMPTY)) {
      if (token == u"render") {
        return true;
      }
    }
  }
  return !element.HasAttribute(u"async") && !element.HasAttribute(u"defer");
}

// Fires a trusted event of `type` at `target`, which does not bubble, as the
// HTML Standard fires load and error at a script.
void FireSimpleEvent(EventTarget& target, const std::u16string& type) {
  bindings::EventInit init;
  Event* const event = Event::Create(type, init);
  event->SetTrusted(true);
  target.DispatchEvent(event);
}

// How many bytes at the end of `bytes` start a UTF-8 sequence that has not
// ended yet.
size_t IncompleteTail(std::string_view bytes) {
  const size_t look = std::min<size_t>(bytes.size(), 3);
  for (size_t back = 1; back <= look; ++back) {
    const uint8_t byte = static_cast<uint8_t>(bytes[bytes.size() - back]);
    if ((byte & 0xC0) == 0x80) {
      // A continuation byte: look further back for its lead.
      continue;
    }
    const size_t length = (byte & 0xE0) == 0xC0   ? 2
                          : (byte & 0xF0) == 0xE0 ? 3
                          : (byte & 0xF8) == 0xF0 ? 4
                                                  : 1;
    return length > back ? back : 0;
  }
  return 0;
}

}  // namespace

Utf8StreamDecoder::Utf8StreamDecoder() = default;
Utf8StreamDecoder::~Utf8StreamDecoder() = default;

std::u16string Utf8StreamDecoder::Decode(std::string_view bytes) {
  pending_.append(bytes);
  std::string_view complete = pending_;
  if (!started_) {
    // The byte order mark, once three bytes have come to tell.
    if (complete.size() < 3 &&
        std::string_view("\xEF\xBB\xBF").starts_with(complete)) {
      return std::u16string();
    }
    started_ = true;
    if (complete.starts_with("\xEF\xBB\xBF")) {
      complete.remove_prefix(3);
    }
  }
  const size_t tail = IncompleteTail(complete);
  std::u16string text;
  base::UTF8ToUTF16(complete.data(), complete.size() - tail, &text);
  pending_ = std::string(complete.substr(complete.size() - tail));
  return text;
}

std::u16string Utf8StreamDecoder::Flush() {
  // A sequence that the stream cut off is not UTF-8, also one that began as a
  // byte order mark.
  std::u16string text;
  base::UTF8ToUTF16(pending_.data(), pending_.size(), &text);
  pending_.clear();
  started_ = true;
  return text;
}

// One classic script that the document fetches through the Session.
class DocumentLoader::ScriptFetch final : public PageRequests::Client {
 public:
  ScriptFetch(DocumentLoader& loader, HTMLScriptElement& element, GURL url)
      : loader_(loader), element_(&element), url_(std::move(url)) {}

  ScriptFetch(const ScriptFetch&) = delete;
  ScriptFetch& operator=(const ScriptFetch&) = delete;

  ~ScriptFetch() override = default;

  void Start() {
    number_ =
        loader_->requests_->Start(PageRequestKind::kScript, url_.spec(), *this,
                                  IsPotentiallyRenderBlocking(*element_));
  }

  bool ended() const { return ended_; }
  // Whether the script has a response with an ok status, and all of its body:
  // the HTML Standard's fetch a classic script.
  bool succeeded() const {
    return net_error_ == kResponseComplete && status_ >= 200 && status_ <= 299;
  }
  HTMLScriptElement& element() const { return *element_; }
  // The URL of the response, after any redirects.
  const GURL& response_url() const { return response_url_; }
  const std::string& response_headers() const { return response_headers_; }
  // The HTML Standard's muted errors: a response of another origin that no
  // CORS check let through, whose errors a window's listeners see nothing of.
  bool muted_errors() const { return opaque_; }
  std::u16string TakeSource() {
    Utf8StreamDecoder decoder;
    std::u16string source = decoder.Decode(source_);
    source += decoder.Flush();
    source_.clear();
    return source;
  }

  // PageRequests::Client:
  void OnResponseStarted(const PageResponseStarted& response) override {
    status_ = response.status;
    response_url_ = GURL(response.url);
    response_headers_ = response.headers;
    opaque_ = response.opaque;
  }
  void OnResponseBody(std::string bytes) override {
    source_ += bytes;
    loader_->requests_->TookBody(number_, bytes.size());
  }
  void OnResponseEnded(int net_error) override {
    ended_ = true;
    net_error_ = net_error;
    // This may delete the fetch.
    loader_->OnScriptFetched(*this);
  }

 private:
  const raw_ref<DocumentLoader> loader_;
  const cppgc::Persistent<HTMLScriptElement> element_;
  const GURL url_;
  uint64_t number_ = 0;
  int status_ = 0;
  GURL response_url_;
  std::string response_headers_;
  bool opaque_ = false;
  std::string source_;
  bool ended_ = false;
  // Until the fetch ends, it has not succeeded.
  int net_error_ = -1;
};

DocumentLoader::DocumentLoader(ScriptRunner& runner,
                               PageRequests& requests,
                               SendFrame send)
    : runner_(runner), requests_(requests), send_(std::move(send)) {}

DocumentLoader::~DocumentLoader() = default;

void DocumentLoader::OnResponseStarted(const PageResponseStarted& response) {
  // The document of the response takes the place of about:blank's, in a
  // context of its own, as Blink makes a new window for a tab's first
  // navigation, whose about:blank has an opaque origin.
  url_ = GURL(response.url);
  // The Session's base::TimeTicks, which the Page's clock shares.
  document_ = &runner_->CommitDocument(
      url_, base::TimeTicks() + base::Microseconds(response.navigation_start),
      response.connection);
  // What DocumentLoader::CommitNavigation() takes from the navigation's
  // timings and response, for the window's Performance Timeline and
  // window.chrome. A restart for Critical-CH is a redirect to where the
  // navigation started, after which the fetch starts again.
  timing_ = &runner_->document_load_timing();
  DocumentLoadTiming& timing = *timing_;
  timing.has_response = true;
  timing.redirect_count = static_cast<uint16_t>(
      response.redirect_count + (response.restarted_for_critical_ch ? 1 : 0));
  timing.has_cross_origin_redirect = response.cross_origin_redirect;
  timing.fetch_start = TicksOf(response.timing.request_start);
  if (timing.redirect_count > 0) {
    timing.redirect_start = TicksOf(response.first_request_start);
    timing.redirect_end = timing.fetch_start;
  }
  timing.critical_ch_restart = TicksOf(response.critical_ch_restart);
  // CreateNavigationTimingInstance(): no start time, the document's URL, and
  // a response that always passes the TAO check, whose cache state the bytes
  // that had come with its headers decide.
  PageResponseStarted navigation_response = response;
  navigation_response.timing_allow_passed = true;
  timing.navigation = CreateResourceTimingInfo(
      base::TimeTicks(), base::UTF8ToUTF16(response.url), &navigation_response,
      base::TimeTicks::Now());
  timing.navigation.cache_state = response.validated ? CacheState::kValidated
                                  : response.received_bytes == 0
                                      ? CacheState::kLocal
                                      : CacheState::kNone;
  timing.connection_info = response.connection_info;
  timing.alpn_negotiated_protocol = response.alpn_negotiated_protocol;
  timing.was_fetched_via_spdy = response.was_fetched_via_spdy;
  timing.was_alpn_negotiated = response.was_alpn_negotiated;
  timing.dom_loading = base::TimeTicks::Now();
  document_->SetReadiness(Document::Readiness::kLoading);
  // Blink's Document::lastModifiedTime(): the response's Last-Modified, where
  // it parses.
  const std::string last_modified =
      HeaderField(response.headers, "last-modified");
  base::Time last_modified_time;
  if (!last_modified.empty() &&
      base::Time::FromString(last_modified.c_str(), &last_modified_time)) {
    document_->SetLastModified(last_modified_time);
  }
  parser_ = std::make_unique<HtmlDocumentParser>(*document_);
}

void DocumentLoader::OnResponseBody(std::string bytes) {
  body_bytes_ += bytes.size();
  waiting_body_.push_back(std::move(bytes));
  Pump();
}

void DocumentLoader::OnResponseTotals(const PageResponseEnded& ended) {
  // WindowPerformance::OnBodyLoadFinished() and FinishedLoading(): the body's
  // sizes, which the navigation's entry shows, and when it ended.
  if (!timing_) {
    return;
  }
  timing_->navigation.encoded_body_size =
      static_cast<uint64_t>(ended.encoded_body_size);
  timing_->navigation.decoded_body_size = body_bytes_;
  timing_->response_end = ended.completion_time != 0
                              ? TicksOf(ended.completion_time)
                              : base::TimeTicks::Now();
}

void DocumentLoader::OnResponseEnded(int net_error) {
  // After a response, a body that stops short still makes a document of what
  // came, as in Chrome.
  body_ended_ = true;
  Pump();
}

void DocumentLoader::Pump() {
  if (!parser_ || blocking_script_ || after_parsing_started_) {
    return;
  }
  while (true) {
    while (HTMLScriptElement* const script = parser_->Pump()) {
      if (PrepareScript(*script)) {
        return;
      }
    }
    if (!waiting_body_.empty()) {
      const std::string bytes = std::move(waiting_body_.front());
      waiting_body_.pop_front();
      parser_->Append(decoder_.Decode(bytes));
      requests_->TookBody(kNavigationRequest, bytes.size());
      continue;
    }
    if (!body_ended_) {
      return;
    }
    if (!parser_finished_) {
      parser_->Append(decoder_.Flush());
      parser_->Finish();
      parser_finished_ = true;
      // The parser may still reach scripts before the end.
      continue;
    }
    RunAfterParsing();
    return;
  }
}

bool DocumentLoader::PrepareScript(HTMLScriptElement& element) {
  const std::optional<std::u16string> src = element.GetAttribute(u"src");
  if (!src && element.Text().empty()) {
    return false;
  }
  if (TypeOf(element) != ScriptType::kClassic) {
    return false;
  }
  // A browser that runs module scripts skips the classic ones meant for
  // browsers that do not, as Chrome does.
  if (element.HasAttribute(u"nomodule")) {
    return false;
  }
  if (!src) {
    runner_->RunClassicScript(element.Text(), url_);
    return false;
  }
  // The document's URL is its base URL: <base> is a Known Gap.
  const GURL url = src->empty() ? GURL() : url_.Resolve(*src);
  if (!url.is_valid()) {
    QueueErrorEvent(element);
    return false;
  }
  auto fetch = std::make_unique<ScriptFetch>(*this, element, url);
  ScriptFetch& started = *fetch;
  const bool runs_after_parsing =
      element.HasAttribute(u"defer") || element.HasAttribute(u"async");
  if (runs_after_parsing) {
    after_parsing_.push_back(std::move(fetch));
  } else {
    blocking_script_ = std::move(fetch);
  }
  started.Start();
  return !runs_after_parsing;
}

void DocumentLoader::ExecuteFetched(ScriptFetch& fetch) {
  HTMLScriptElement& element = fetch.element();
  if (!fetch.succeeded()) {
    FireSimpleEvent(element, u"error");
    return;
  }
  // A response whose MIME type cannot run as a script is a network error, and
  // Chrome's console says why.
  if (std::optional<std::string> refused =
          RefusedAsScript(fetch.response_url(), fetch.response_headers())) {
    PageConsoleMessage message;
    message.level = ConsoleLevel::kError;
    message.text = *std::move(refused);
    send_.Run(EncodeConsole(message));
    FireSimpleEvent(element, u"error");
    return;
  }
  runner_->RunClassicScript(fetch.TakeSource(), fetch.response_url(),
                            fetch.muted_errors());
  FireSimpleEvent(element, u"load");
}

void DocumentLoader::OnScriptFetched(ScriptFetch& fetch) {
  if (&fetch == blocking_script_.get()) {
    const std::unique_ptr<ScriptFetch> done = std::move(blocking_script_);
    ExecuteFetched(*done);
    Pump();
    return;
  }
  // A script that runs after parsing waits for those before it, and for the
  // parser to finish, which then runs it.
  if (after_parsing_started_) {
    RunAfterParsing();
  }
}

void DocumentLoader::RunAfterParsing() {
  if (!after_parsing_started_) {
    after_parsing_started_ = true;
    timing_->dom_interactive = base::TimeTicks::Now();
    document_->SetReadiness(Document::Readiness::kInteractive);
    // Document::SetReadyState() tells Chrome's browser process, which answers
    // with the navigation's confidence from a later task: high, as for any
    // navigation once the browser has started, with its noise.
    runner_->PostTask(base::BindOnce(
        [](const cppgc::Persistent<DocumentLoadTiming>& timing) {
          timing->high_confidence =
              PerformanceTimingConfidence::DrawHighConfidence();
        },
        timing_));
  }
  while (!after_parsing_.empty()) {
    if (!after_parsing_.front()->ended()) {
      // OnScriptFetched() comes back here.
      return;
    }
    const std::unique_ptr<ScriptFetch> next = std::move(after_parsing_.front());
    after_parsing_.pop_front();
    ExecuteFetched(*next);
  }
  if (dom_content_loaded_queued_) {
    return;
  }
  dom_content_loaded_queued_ = true;
  runner_->PostTask(base::BindOnce(&DocumentLoader::FireDOMContentLoaded,
                                   weak_factory_.GetWeakPtr()));
}

void DocumentLoader::FireDOMContentLoaded() {
  bindings::EventInit init;
  init.bubbles = true;
  Event* const event = Event::Create(u"DOMContentLoaded", init);
  event->SetTrusted(true);
  timing_->dom_content_loaded_event_start = base::TimeTicks::Now();
  // It reaches the document's listeners. Events that go through the tree and
  // on to the window are a Known Gap.
  document_->DispatchEvent(event);
  timing_->dom_content_loaded_event_end = base::TimeTicks::Now();
  // Nothing else that a document loads holds its load event back yet.
  runner_->PostTask(
      base::BindOnce(&DocumentLoader::FireLoad, weak_factory_.GetWeakPtr()));
}

void DocumentLoader::FireLoad() {
  timing_->dom_complete = base::TimeTicks::Now();
  document_->SetReadiness(Document::Readiness::kComplete);
  bindings::EventInit init;
  Event* const event = Event::Create(u"load", init);
  event->SetTrusted(true);
  timing_->load_event_start = base::TimeTicks::Now();
  if (Window* const window = document_->DefaultView()) {
    window->DispatchEventWithTarget(event, document_.Get());
  }
  timing_->load_event_end = base::TimeTicks::Now();
  send_.Run(EncodeLoaded());
}

void DocumentLoader::QueueErrorEvent(HTMLScriptElement& element) {
  runner_->PostTask(base::BindOnce(
      [](const cppgc::Persistent<HTMLScriptElement>& element) {
        FireSimpleEvent(*element, u"error");
      },
      cppgc::Persistent<HTMLScriptElement>(&element)));
}

}  // namespace mitto
