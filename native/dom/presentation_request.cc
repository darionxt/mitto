#include "mitto/dom/presentation_request.h"

#include <string>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_presentation_request.h"
#include "mitto/dom/document.h"
#include "mitto/dom/presentation_availability.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/window.h"
#include "net/base/url_util.h"
#include "url/url_constants.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// network::IsUrlPotentiallyTrustworthy(): a secure scheme, or a host that is
// the machine itself.
bool IsUrlPotentiallyTrustworthy(const GURL& url) {
  if (url.SchemeIs(url::kHttpsScheme) || url.SchemeIs(url::kWssScheme)) {
    return true;
  }
  return (url.SchemeIs(url::kHttpScheme) || url.SchemeIs(url::kWsScheme)) &&
         net::IsLocalhost(url);
}

// MixedContentChecker::IsMixedContent(): a URL that is not potentially
// trustworthy, from a document whose scheme restricts mixed content.
bool IsMixedContent(const GURL& document, const GURL& url) {
  return document.SchemeIs(url::kHttpsScheme) &&
         !IsUrlPotentiallyTrustworthy(url);
}

bool IsKnownProtocolForPresentationUrl(const GURL& url) {
  return url.SchemeIsHTTPOrHTTPS() || url.SchemeIs("cast") ||
         url.SchemeIs("cast-dial");
}

}  // namespace

// static
PresentationRequest* PresentationRequest::Create(
    const std::u16string& url,
    bindings::ExceptionState& exception_state) {
  return Create(std::vector<std::u16string>{url}, exception_state);
}

// static
PresentationRequest* PresentationRequest::Create(
    const std::vector<std::u16string>& urls,
    bindings::ExceptionState& exception_state) {
  const GURL document(
      base::UTF16ToUTF8(Window::Current().GetDocument().UrlForBinding()));
  std::vector<GURL> parsed_urls;
  for (const std::u16string& url : urls) {
    // KURL(execution_context->Url(), url).
    const GURL parsed = document.Resolve(url);
    if (!parsed.is_valid()) {
      exception_state.ThrowDOMException(
          "SyntaxError", base::StrCat({"'", base::UTF16ToUTF8(url),
                                       "' can't be resolved to a valid URL."}));
      return nullptr;
    }
    if (parsed.SchemeIsHTTPOrHTTPS() && IsMixedContent(document, parsed)) {
      exception_state.ThrowDOMException(
          "SecurityError",
          base::StrCat({"Presentation of an insecure document [",
                        base::UTF16ToUTF8(url),
                        "] is prohibited from a secure context."}));
      return nullptr;
    }
    if (IsKnownProtocolForPresentationUrl(parsed)) {
      parsed_urls.push_back(parsed);
    }
  }
  if (parsed_urls.empty()) {
    exception_state.ThrowDOMException(
        "NotSupportedError", "An empty sequence of URLs is not supported.");
    return nullptr;
  }
  return bindings::MakeGarbageCollected<PresentationRequest>(
      std::move(parsed_urls));
}

PresentationRequest::PresentationRequest(std::vector<GURL> urls)
    : urls_(std::move(urls)) {}

PresentationRequest::~PresentationRequest() = default;

const bindings::WrapperTypeInfo* PresentationRequest::GetWrapperTypeInfo()
    const {
  return &bindings::V8PresentationRequest::kWrapperTypeInfo;
}

void PresentationRequest::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(availability_);
}

v8::Local<v8::Promise> PresentationRequest::Reconnect(
    const std::u16string& id,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // The browser has no presentation of that ID to reconnect to, and
  // PresentationConnectionCallbacks rejects with what it says.
  RejectWithDOMExceptionFromTask(isolate, resolver, "NotFoundError",
                                 "Route not found");
  return resolver->GetPromise();
}

v8::Local<v8::Promise> PresentationRequest::GetAvailability(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  if (!availability_) {
    availability_ = PresentationAvailability::Create();
  }
  if (availability_known_) {
    std::ignore = resolver->Resolve(isolate->GetCurrentContext(),
                                    availability_->ToV8(isolate));
    return resolver->GetPromise();
  }
  // The first requests wait for the browser, which finds no display: the
  // availability is false then, and later requests settle at once.
  // The availability stays alive until the task settles the promise with it,
  // as Blink's resolver holds its result, whether or not the request does.
  ResolveFromTaskWith(
      isolate, resolver,
      base::BindOnce(
          [](cppgc::WeakPersistent<PresentationRequest> request,
             cppgc::Persistent<PresentationAvailability> availability,
             v8::Isolate* isolate) -> v8::Local<v8::Value> {
            if (request) {
              request->availability_known_ = true;
            }
            return availability->ToV8(isolate);
          },
          cppgc::WeakPersistent<PresentationRequest>(this),
          cppgc::Persistent<PresentationAvailability>(availability_.Get())));
  return resolver->GetPromise();
}

}  // namespace mitto
