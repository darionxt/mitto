#include "mitto/dom/pending_script.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/encoding/text_resource_decoder_builder.h"
#include "mitto/header_rules.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-message.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"

namespace mitto {
namespace {

// The HTML Standard's JavaScript MIME types, which Blink's
// MIMETypeRegistry::IsSupportedJavaScriptMIMEType() holds.
// Upstream: third_party/blink/renderer/platform/network/mime/mime_type_registry.cc
bool IsJavaScriptMimeType(std::string_view mime_type) {
  static constexpr std::array<std::string_view, 16> kJavaScriptMimeTypes = {
      "application/ecmascript",
      "application/javascript",
      "application/x-ecmascript",
      "application/x-javascript",
      "text/ecmascript",
      "text/javascript",
      "text/javascript1.0",
      "text/javascript1.1",
      "text/javascript1.2",
      "text/javascript1.3",
      "text/javascript1.4",
      "text/javascript1.5",
      "text/jscript",
      "text/livescript",
      "text/x-ecmascript",
      "text/x-javascript",
  };
  for (const std::string_view javascript : kJavaScriptMimeTypes) {
    if (mime_type == javascript) {
      return true;
    }
  }
  return false;
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

// ClassicScript::RunScript() in the current context: compiles and runs
// `source` as a classic script named `source_url`, reports what it throws as
// an uncaught error, and runs the microtasks that it queued once it returns,
// if no script runs beneath it, as the HTML Standard cleans up after running
// a script.
void RunClassicScript(std::u16string_view source,
                      const GURL& source_url,
                      int start_line,
                      int start_column,
                      bool muted_errors) {
  // V8 holds no longer string, and a script that it cannot hold does not run.
  if (source.size() > static_cast<size_t>(v8::String::kMaxLength)) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> text;
  v8::Local<v8::String> name;
  const std::string& spec = source_url.possibly_invalid_spec();
  if (!v8::String::NewFromTwoByte(
           isolate, reinterpret_cast<const uint16_t*>(source.data()),
           v8::NewStringType::kNormal, base::checked_cast<int>(source.size()))
           .ToLocal(&text) ||
      !v8::String::NewFromUtf8(isolate, spec.data(), v8::NewStringType::kNormal,
                               base::checked_cast<int>(spec.size()))
           .ToLocal(&name)) {
    return;
  }
  // A script of another origin that came without CORS has its errors muted.
  v8::ScriptOrigin origin =
      ClassicScriptOrigin(name, muted_errors, start_line, start_column);
  v8::Local<v8::Script> compiled;
  if (!v8::Script::Compile(context, text, &origin).ToLocal(&compiled) ||
      compiled->Run(context).IsEmpty()) {
    ReportScriptException(isolate, try_catch);
  }
}

// SecurityOrigin::IsSameSiteWith(): the same scheme and registrable domain,
// or host where there is none.
bool IsSameSite(const GURL& a, const GURL& b) {
  return a.scheme() == b.scheme() &&
         net::registry_controlled_domains::SameDomainOrHost(
             a, b,
             net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

}  // namespace

// Upstream: third_party/blink/renderer/platform/weborigin/kurl.cc
std::string ElidedUrl(const GURL& url) {
  const std::string& spec = url.possibly_invalid_spec();
  if (spec.size() <= 1024) {
    return spec;
  }
  return base::StrCat(
      {spec.substr(0, 511), "...", spec.substr(spec.size() - 510)});
}

// Upstream: third_party/blink/renderer/platform/loader/allowed_by_nosniff.cc
std::optional<std::string> RefusedAsScript(const GURL& url,
                                           std::string_view headers) {
  const std::string mime_type = HttpContentType(headers);
  const bool is_javascript = IsJavaScriptMimeType(mime_type);
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

// What a script's fetch hears, which keeps the script until the fetch ends.
class PendingScript::FetchClient final : public mitto::FetchClient {
 public:
  explicit FetchClient(PendingScript& script) : script_(&script) {}

  FetchClient(const FetchClient&) = delete;
  FetchClient& operator=(const FetchClient&) = delete;

  ~FetchClient() override = default;

  // FetchClient:
  void OnStarted(FetchController& controller) override {
    controller_ = &controller;
  }
  void OnResponseStarted(const FetchResponseHead& head) override {
    status_ = head.status;
    response_url_ = GURL(head.url);
    headers_ = head.headers;
    opaque_ = head.opaque;
  }
  void OnResponseBody(std::string bytes) override {
    body_ += bytes;
    // A script's body is taken as it comes.
    controller_->TookBody(bytes.size());
  }
  void OnResponseEnded(int net_error) override {
    script_->NotifyFinished(net_error, status_, response_url_, headers_,
                            opaque_, std::move(body_));
  }

 private:
  const cppgc::Persistent<PendingScript> script_;
  raw_ptr<FetchController> controller_ = nullptr;
  int status_ = 0;
  GURL response_url_;
  std::string headers_;
  bool opaque_ = false;
  std::string body_;
};

// static
PendingScript* PendingScript::CreateInline(HTMLScriptElement& element,
                                           std::u16string source,
                                           GURL source_url,
                                           int start_line,
                                           int start_column) {
  PendingScript* const script = bindings::MakeGarbageCollected<PendingScript>(
      element, /*is_external=*/false);
  script->source_ = std::move(source);
  script->source_url_ = std::move(source_url);
  script->start_line_ = start_line;
  script->start_column_ = start_column;
  return script;
}

// static
PendingScript* PendingScript::Fetch(HTMLScriptElement& element,
                                    const GURL& url,
                                    FetchRequestData request,
                                    IntegrityMetadataSet integrity,
                                    std::string encoding,
                                    bool render_blocking) {
  PendingScript* const script = bindings::MakeGarbageCollected<PendingScript>(
      element, /*is_external=*/true);
  script->integrity_ = std::move(integrity);
  script->encoding_ = std::move(encoding);
  script->StartFetch(url, std::move(request), render_blocking);
  return script;
}

PendingScript::PendingScript(HTMLScriptElement& element, bool is_external)
    : element_(&element),
      original_element_document_(&element.GetDocument()),
      original_window_(element.GetDocument().DefaultView()),
      is_external_(is_external),
      state_(is_external ? State::kWaitingForResource : State::kReady) {}

PendingScript::~PendingScript() = default;

void PendingScript::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(element_);
  visitor->Trace(original_element_document_);
  visitor->Trace(original_window_);
}

void PendingScript::StartFetch(const GURL& url,
                               FetchRequestData request,
                               bool render_blocking) {
  fetch_url_ = url;
  Window* const window = original_window_.Get();
  Fetcher* const fetcher = window ? window->GetFetcher() : nullptr;
  if (!fetcher) {
    // A window with no Session to fetch through, as the Page program's
    // given a script.
    state_ = State::kErrorOccurred;
    return;
  }
  request.destination = FetchDestination::kScript;
  request.method = "GET";
  request.url = url.spec();
  request.render_blocking = render_blocking;
  fetcher->Fetch(std::move(request), std::make_unique<FetchClient>(*this));
}

void PendingScript::WatchForLoad(base::OnceClosure on_ready) {
  DCHECK(!IsWatchingForLoad());
  on_ready_ = std::move(on_ready);
  if (IsReady()) {
    std::move(on_ready_).Run();
  }
}

bool PendingScript::IsEligibleForLowPriorityAsyncScriptExecution() const {
  DCHECK_EQ(scheduling_type_, ScriptSchedulingType::kAsync);
  const Document* const document = original_element_document_.Get();
  const Window* const window = original_window_.Get();
  if (!document || !window || !document->IsHTMLDocument() || !element_) {
    return false;
  }
  // kLowPriorityAsyncScriptExecution, on by default but on ChromeOS, for a
  // machine that is not a low-end one and has 3 GB or more.
  if (window->Start().memory < 3) {
    return false;
  }
  // Within 3 seconds of the document's start, for a script of another site,
  // that does not block rendering.
  if (base::TimeTicks::Now() - document->StartTime() > base::Seconds(3)) {
    return false;
  }
  if (IsSameSite(fetch_url_, GURL(document->UrlForBinding()))) {
    return false;
  }
  return !element_->IsPotentiallyRenderBlocking();
}

void PendingScript::NotifyFinished(int net_error,
                                   int status,
                                   const GURL& response_url,
                                   const std::string& headers,
                                   bool opaque,
                                   std::string body) {
  // Each Window's console says the errors of its documents' scripts.
  Window* const window = original_window_.Get();
  Document* const document = original_element_document_.Get();
  // A context or a document that has gone makes it a network error, which
  // nothing can see.
  if (!window || !document || &window->GetDocument() != document) {
    AdvanceReadyState(State::kErrorOccurred);
    return;
  }
  // The HTML Standard's fetch a classic script: a response with an ok status
  // and all of its body.
  if (net_error != 0 || status < 200 || status > 299) {
    AdvanceReadyState(State::kErrorOccurred);
    return;
  }
  // Subresource Integrity, then what the Fetch Standard blocks for its MIME
  // type, are network errors too, and the console says why.
  std::vector<std::string> messages;
  const bool integrity_failed =
      !integrity_.empty() &&
      !CheckSubresourceIntegrity(integrity_, body, ElidedUrl(response_url),
                                 !opaque, headers, messages);
  for (std::string& message : messages) {
    window->AddConsoleMessage(ConsoleLevel::kError, std::move(message));
  }
  std::optional<std::string> refused = RefusedAsScript(response_url, headers);
  if (refused) {
    window->AddConsoleMessage(ConsoleLevel::kError, *std::move(refused));
  }
  if (integrity_failed || refused) {
    AdvanceReadyState(State::kErrorOccurred);
    return;
  }
  // ScriptResource's decoder.
  const std::unique_ptr<TextResourceDecoder> decoder =
      BuildClassicScriptDecoder(
          encoding_,
          GetMimeTypeAndCharset(HeaderField(headers, "content-type")).charset);
  source_ = decoder->Decode(base::as_byte_span(body));
  source_ += decoder->Flush();
  source_url_ = response_url;
  muted_errors_ = opaque;
  AdvanceReadyState(State::kReady);
}

void PendingScript::AdvanceReadyState(State state) {
  DCHECK_EQ(state_, State::kWaitingForResource);
  state_ = state;
  if (IsWatchingForLoad()) {
    std::move(on_ready_).Run();
  }
}

void PendingScript::ExecuteScriptBlock() {
  HTMLScriptElement* const element = element_.Get();
  const State state = state_;
  const bool is_external = is_external_;
  const bool muted_errors = muted_errors_;
  std::u16string source = std::move(source_);
  const GURL source_url = source_url_;
  const int start_line = start_line_;
  const int start_column = start_column_;
  Window* const original_window = original_window_.Get();
  Document* const original_document = original_element_document_.Get();
  Dispose();
  if (!element) {
    return;
  }
  // A script that has moved to another context or document does not run.
  Document& document = element->GetDocument();
  Window* const window = document.DefaultView();
  if (!window || window != original_window || &document != original_document) {
    return;
  }
  // Steps 2 to 6 of execute the script element.
  if (state == State::kErrorOccurred) {
    element->DispatchErrorEvent();
    return;
  }
  document.PushCurrentScript(element);
  RunClassicScript(source, source_url, start_line, start_column, muted_errors);
  document.PopCurrentScript(element);
  if (is_external) {
    element->DispatchLoadEvent();
  }
}

void PendingScript::Dispose() {
  StopWatchingForLoad();
  element_ = nullptr;
}

}  // namespace mitto
