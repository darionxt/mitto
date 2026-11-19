#include "mitto/dom/module_script_loader.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/data_url.h"
#include "mitto/dom/document.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "mitto/header_rules.h"
#include "url/origin.h"
#include "url/url_constants.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// MIMETypeRegistry::IsSupportedJavaScriptMIMEType(), for a MIME type that is
// already in lower case.
bool IsSupportedJavaScriptMimeType(std::string_view mime_type) {
  static constexpr std::string_view kJavaScriptMimeTypes[] = {
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

// IsJSONMimeType(), for a MIME type in lower case without parameters, as
// net::MatchesMimeType() takes its patterns: application/json, text/json, or
// a type and subtype that are both tokens, whose subtype ends in +json.
// Upstream: third_party/blink/common/mime_util/mime_util.cc
// Upstream: net/base/mime_util.cc
bool IsJsonMimeType(std::string_view mime_type) {
  if (mime_type == "application/json" || mime_type == "text/json") {
    return true;
  }
  const size_t slash = mime_type.find('/');
  return slash != std::string_view::npos &&
         IsHttpToken(mime_type.substr(0, slash)) &&
         IsHttpToken(mime_type.substr(slash + 1)) &&
         mime_type.ends_with("+json");
}

// The destination that a module of `type` is fetched as: the fetch
// destination from module type, as kFetchDestinationJsonCssModules has it.
FetchDestination DestinationOf(ModuleType type) {
  switch (type) {
    case ModuleType::kJSON:
      return FetchDestination::kJson;
    case ModuleType::kCSS:
      return FetchDestination::kStyle;
    case ModuleType::kText:
      return FetchDestination::kText;
    case ModuleType::kJavaScriptOrWasm:
    case ModuleType::kInvalid:
      return FetchDestination::kScript;
  }
}

}  // namespace

// Upstream: third_party/blink/renderer/core/loader/modulescript/module_script_fetcher.cc
std::optional<ResolvedModuleType> ResolveFetchedModuleType(
    const std::string& headers,
    ModuleType expected,
    std::string* message) {
  // The type that the response says, which no sniffing rewrites.
  const std::string mime_type = HttpContentType(headers);
  if (expected == ModuleType::kJavaScriptOrWasm &&
      IsSupportedJavaScriptMimeType(mime_type)) {
    return ResolvedModuleType::kJavaScript;
  }
  if (expected == ModuleType::kJSON && IsJsonMimeType(mime_type)) {
    return ResolvedModuleType::kJSON;
  }
  if (expected == ModuleType::kCSS && mime_type == "text/css") {
    return ResolvedModuleType::kCSS;
  }
  if (expected == ModuleType::kText) {
    return ResolvedModuleType::kText;
  }
  *message = base::StrCat(
      {"Failed to load module script: Expected a ",
       ModuleScriptCreationParams::ModuleTypeToString(expected),
       " module script but the server responded with a MIME type of \"",
       HeaderBytesToUtf8(mime_type),
       "\". Strict MIME type checking is enforced for module scripts per HTML "
       "spec."});
  return std::nullopt;
}

// What a module's fetch hears, which keeps the loader until the fetch ends.
class ModuleScriptLoader::FetchClient final : public mitto::FetchClient {
 public:
  explicit FetchClient(ModuleScriptLoader& loader) : loader_(&loader) {}

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
    controller_->TookBody(bytes.size());
  }
  void OnResponseEnded(int net_error) override {
    loader_->NotifyFetchFinished(net_error, status_, response_url_, headers_,
                                 opaque_, std::move(body_));
  }

 private:
  const cppgc::Persistent<ModuleScriptLoader> loader_;
  raw_ptr<FetchController> controller_ = nullptr;
  int status_ = 0;
  GURL response_url_;
  std::string headers_;
  bool opaque_ = false;
  std::string body_;
};

// static
void ModuleScriptLoader::Fetch(const ModuleScriptFetchRequest& request,
                               ModuleGraphLevel level,
                               Modulator& modulator,
                               ModuleScriptLoaderClient& client) {
  ModuleScriptLoader* const loader =
      bindings::MakeGarbageCollected<ModuleScriptLoader>(
          modulator, request.options, client);
  loader->FetchInternal(request, level);
}

ModuleScriptLoader::ModuleScriptLoader(Modulator& modulator,
                                       const ScriptFetchOptions& options,
                                       ModuleScriptLoaderClient& client)
    : modulator_(&modulator), options_(options), client_(&client) {}

ModuleScriptLoader::~ModuleScriptLoader() = default;

void ModuleScriptLoader::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(modulator_);
  visitor->Trace(client_);
}

void ModuleScriptLoader::FetchInternal(const ModuleScriptFetchRequest& request,
                                       ModuleGraphLevel level) {
  url_ = request.url;
  expected_module_type_ = request.expected_module_type;
  // A data: URL is answered in the Page, with no request, as Blink's
  // ResourceFetcher answers one from its static data, and one that does not
  // parse fails as its loader would.
  if (request.url.SchemeIs(url::kDataScheme)) {
    std::optional<DataUrlResponse> response = ParseDataUrl(request.url);
    if (!response) {
      Finish(nullptr);
      return;
    }
    NotifyFetchFinished(
        /*net_error=*/0, /*status=*/200, request.url,
        base::StrCat({"Content-Type: ", response->content_type, "\n"}),
        /*opaque=*/false, std::move(response->body));
    return;
  }
  Window* const window = modulator_->GetWindow();
  Fetcher* const fetcher = window ? window->GetFetcher() : nullptr;
  if (!fetcher) {
    // A window with no Session to fetch through, as the Page program's
    // given a script: a network error.
    Finish(nullptr);
    return;
  }
  // Step 8 to 12: a request of mode cors, with the credentials mode, the
  // referrer policy and the integrity of the options, and the referrer that
  // the caller gives, of the destination of its module type.
  FetchRequestData data;
  data.method = "GET";
  data.url = request.url.spec();
  data.destination = DestinationOf(request.expected_module_type);
  data.is_module = true;
  data.mode = FetchMode::kCors;
  data.credentials = options_.credentials_mode;
  data.referrer_policy = request.options.referrer_policy;
  // The referrer is the base URL of the script that imports it, which the
  // Session takes from the document's origin, or as the final URL of a
  // response that the document has had, such as a module's of another
  // origin. One that is neither, as the base URL of an inline script beneath
  // a base element of another origin, is the document's URL: a Known Gap.
  if (request.referrer == kNoReferrer) {
    data.referrer = kNoReferrer;
  } else if (request.referrer != kClientReferrer) {
    const GURL referrer(request.referrer);
    if (referrer.is_valid() &&
        (url::Origin::Create(referrer).IsSameOriginWith(
             GURL(window->GetDocument().UrlForBinding())) ||
         fetcher->HasFinishedResponse(referrer.spec()))) {
      data.referrer = referrer.spec();
    }
  }
  // A module script's priority is its destination's whatever its scheduling,
  // which only its fetchpriority moves, as ResourceFetcher holds the lazy
  // load's low priority to classic scripts.
  data.priority = ComputeFetchPriority(
      /*lazy=*/false, options_.fetch_priority_hint,
      options_.render_blocking == RenderBlockingBehavior::kBlocking);
  data.render_blocking =
      options_.render_blocking == RenderBlockingBehavior::kBlocking;
  fetcher->Fetch(std::move(data), std::make_unique<FetchClient>(*this));
}

void ModuleScriptLoader::NotifyFetchFinished(int net_error,
                                             int status,
                                             const GURL& response_url,
                                             const std::string& headers,
                                             bool opaque,
                                             std::string body) {
  // Abort the steps if the browsing context is discarded.
  Window* const window = modulator_->GetWindow();
  if (!modulator_->HasValidContext() || !window) {
    Finish(nullptr);
    return;
  }
  // Step 13.1, WasModuleLoadSuccessful(): the integrity check's errors
  // first, then a body that failed or did not pass integrity, and a status
  // that is not ok.
  std::vector<std::string> messages;
  bool integrity_failed = false;
  if (net_error == 0 && !options_.integrity_metadata.empty()) {
    integrity_failed = !CheckSubresourceIntegrity(options_.integrity_metadata,
                                                  body, ElidedUrl(response_url),
                                                  !opaque, headers, messages);
  }
  for (std::string& message : messages) {
    window->AddConsoleMessage(ConsoleLevel::kError, std::move(message));
  }
  if (net_error != 0 || integrity_failed || status < 200 || status > 299) {
    Finish(nullptr);
    return;
  }
  std::string mime_message;
  const std::optional<ResolvedModuleType> resolved_type =
      ResolveFetchedModuleType(headers, expected_module_type_, &mime_message);
  if (!resolved_type) {
    window->AddConsoleMessage(ConsoleLevel::kError, std::move(mime_message));
    Finish(nullptr);
    return;
  }
  // DocumentModuleScriptFetcher::NotifyFinished(): an external module
  // script's base URL is its source URL, the response's, decoded as UTF-8.
  ModuleScriptCreationParams params;
  params.source_url = response_url;
  params.base_url = response_url;
  params.module_type = *resolved_type;
  TextResourceDecoder decoder(TextResourceDecoderOptions::CreateUtf8Decode());
  params.source_text = decoder.Decode(base::as_byte_span(body));
  params.source_text += decoder.Flush();
  // Step 12.5: the response's Referrer-Policy, as its fetches go on with it.
  const std::string referrer_policy_header =
      HeaderField(headers, "referrer-policy");
  if (!referrer_policy_header.empty()) {
    params.response_referrer_policy =
        FetchReferrerPolicyFromHeaderValue(referrer_policy_header);
  }
  if (params.response_referrer_policy != FetchReferrerPolicy::kDefault) {
    options_.referrer_policy = params.response_referrer_policy;
  }
  switch (params.module_type) {
    case ResolvedModuleType::kJSON:
      Finish(
          ValueWrapperSyntheticModuleScript::
              CreateJSONWrapperSyntheticModuleScript(params, modulator_.Get()));
      return;
    case ResolvedModuleType::kText:
      Finish(
          ValueWrapperSyntheticModuleScript::
              CreateTextWrapperSyntheticModuleScript(params, modulator_.Get()));
      return;
    case ResolvedModuleType::kCSS:
      // A CSS module script's CSSStyleSheet comes with M9's CSSOM: a Known
      // Gap, which fails as a fetch does.
      Finish(nullptr);
      return;
    case ResolvedModuleType::kJavaScript:
      Finish(JSModuleScript::Create(params, modulator_.Get(), options_));
      return;
  }
}

void ModuleScriptLoader::Finish(ModuleScript* module_script) {
  CHECK(!finished_);
  finished_ = true;
  client_->NotifyNewSingleModuleFinished(module_script);
}

}  // namespace mitto
