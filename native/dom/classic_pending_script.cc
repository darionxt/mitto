#include "mitto/dom/classic_pending_script.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/referrer_script_info.h"
#include "mitto/dom/script_element_base.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/encoding/text_resource_decoder_builder.h"
#include "mitto/header_rules.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/url_constants.h"
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
// `source` as a classic script named `source_url`, whose import() calls
// resolve against `base_url` with `options`, reports what it throws as an
// uncaught error, and runs the microtasks that it queued once it returns, if
// no script runs beneath it, as the HTML Standard cleans up after running a
// script.
void RunClassicScript(std::u16string_view source,
                      const GURL& source_url,
                      const GURL& base_url,
                      const ScriptFetchOptions& options,
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
      ClassicScriptOrigin(name, muted_errors, start_line, start_column,
                          ReferrerScriptInfo(base_url, options)
                              .ToV8HostDefinedOptions(isolate, source_url));
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

// Upstream: third_party/blink/renderer/platform/loader/allowed_by_nosniff.cc
std::optional<std::string> RefusedAsScript(const GURL& url,
                                           std::string_view headers) {
  const std::string mime_type = HttpContentType(headers);
  const bool is_javascript = IsJavaScriptMimeType(mime_type);
  if (IsNosniff(headers) && !is_javascript) {
    return base::StrCat({"Refused to execute script from '", ElidedUrl(url),
                         "' because its MIME type ('",
                         HeaderBytesToUtf8(mime_type),
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
                           "' because its MIME type ('",
                           HeaderBytesToUtf8(mime_type),
                           "') is not executable."});
    }
  }
  if (mime_type.starts_with("text/csv")) {
    return base::StrCat({"Refused to execute script from '", ElidedUrl(url),
                         "' because its MIME type ('",
                         HeaderBytesToUtf8(mime_type),
                         "') is not executable."});
  }
  return std::nullopt;
}

// static
ClassicPendingScript* ClassicPendingScript::CreateInline(
    ScriptElementBase& element,
    std::u16string source,
    GURL source_url,
    GURL base_url,
    const ScriptFetchOptions& options,
    int start_line,
    int start_column) {
  ClassicPendingScript* const script =
      bindings::MakeGarbageCollected<ClassicPendingScript>(
          element, /*is_external=*/false, options);
  script->source_ = std::move(source);
  script->source_url_ = std::move(source_url);
  script->base_url_ = std::move(base_url);
  script->start_line_ = start_line;
  script->start_column_ = start_column;
  return script;
}

// static
ClassicPendingScript* ClassicPendingScript::Fetch(
    ScriptElementBase& element,
    const FetchParameters& params,
    const ScriptFetchOptions& options) {
  ClassicPendingScript* const script =
      bindings::MakeGarbageCollected<ClassicPendingScript>(
          element, /*is_external=*/true, options);
  script->integrity_ = params.integrity;
  script->StartFetch(params);
  return script;
}

ClassicPendingScript::ClassicPendingScript(ScriptElementBase& element,
                                           bool is_external,
                                           const ScriptFetchOptions& options)
    : PendingScript(element),
      is_external_(is_external),
      state_(is_external ? State::kWaitingForResource : State::kReady),
      options_(options) {}

ClassicPendingScript::~ClassicPendingScript() = default;

void ClassicPendingScript::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(resource_);
  PendingScript::Trace(visitor);
}

void ClassicPendingScript::StartFetch(const FetchParameters& params) {
  fetch_url_ = params.url;
  Document* const document = original_element_document();
  Resource* const resource = document && original_window()
                                 ? document->Fetcher().RequestResource(params)
                                 : nullptr;
  if (!resource) {
    // A window with no Session to fetch through, as the Page program's
    // given a script.
    state_ = State::kErrorOccurred;
    return;
  }
  resource_ = resource;
  // A resource that has ended tells the script at once.
  resource->AddClient(this);
}

void ClassicPendingScript::DisposeInternal() {
  if (Resource* const resource = resource_.Get()) {
    resource_ = nullptr;
    resource->RemoveClient(this);
  }
}

bool ClassicPendingScript::IsExternal() const {
  return is_external_;
}

bool ClassicPendingScript::IsReady() const {
  return state_ != State::kWaitingForResource;
}

bool ClassicPendingScript::HasScript() const {
  return state_ == State::kReady;
}

bool ClassicPendingScript::IsModule() const {
  return false;
}

bool ClassicPendingScript::IsEligibleForLowPriorityAsyncScriptExecution()
    const {
  DCHECK_EQ(scheduling_type(), ScriptSchedulingType::kAsync);
  const Document* const document = original_element_document();
  const Window* const window = original_window();
  if (!document || !window || !document->IsHTMLDocument() || !element()) {
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
  return !ToScriptElement(*element())->IsPotentiallyRenderBlocking();
}

void ClassicPendingScript::NotifyFinished(Resource& resource) {
  if (&resource != resource_.Get() || state_ != State::kWaitingForResource) {
    return;
  }
  resource.RemoveClient(this);
  const int status = resource.HttpStatus();
  const GURL response_url(resource.Response().url);
  const std::string& headers = resource.Response().headers;
  const bool opaque = resource.Response().opaque;
  const std::string& body = resource.Body();
  // Each Window's console says the errors of its documents' scripts.
  Window* const window = original_window();
  Document* const document = original_element_document();
  // A context or a document that has gone makes it a network error, which
  // nothing can see.
  if (!window || !document || &window->GetDocument() != document) {
    AdvanceReadyState(State::kErrorOccurred);
    return;
  }
  // The HTML Standard's fetch a classic script: a response with an ok status
  // and all of its body.
  if (resource.ErrorOccurred() || status < 200 || status > 299) {
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
  // ScriptResource's decoder, of the request that made the resource.
  const std::unique_ptr<TextResourceDecoder> decoder =
      BuildClassicScriptDecoder(
          resource.Charset(),
          GetMimeTypeAndCharset(HeaderField(headers, "content-type")).charset);
  source_ = decoder->Decode(base::as_byte_span(body));
  source_ += decoder->Flush();
  // ClassicScript::CreateFromResource(): the script is named after the URL
  // it was fetched from, not one it was redirected to, without a fragment.
  // Its base URL is the response's, or about:blank for one whose errors are
  // muted, as SanitizeBaseUrl() has it.
  GURL::Replacements no_fragment;
  no_fragment.ClearRef();
  source_url_ = fetch_url_.ReplaceComponents(no_fragment);
  base_url_ = opaque ? GURL(url::kAboutBlankURL) : response_url;
  muted_errors_ = opaque;
  AdvanceReadyState(State::kReady);
}

void ClassicPendingScript::AdvanceReadyState(State state) {
  DCHECK_EQ(state_, State::kWaitingForResource);
  state_ = state;
  PendingScriptFinished();
}

void ClassicPendingScript::RunScript() {
  const std::u16string source = std::move(source_);
  RunClassicScript(source, source_url_, base_url_, options_, start_line_,
                   start_column_, muted_errors_);
}

}  // namespace mitto
