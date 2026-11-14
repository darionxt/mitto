#include "mitto/dom/location.h"

#include <optional>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_location.h"
#include "mitto/dom/document.h"
#include "mitto/dom/resolved_url.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace mitto {

// static
Location* Location::Create(Window* window) {
  return bindings::MakeGarbageCollected<Location>(window);
}

// static
bindings::ScriptWrappable* Location::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<Location>(
      info, bindings::V8Location::kWrapperTypeInfo, nullptr);
}

Location::Location(Window* window) : window_(window) {}

Location::~Location() = default;

const bindings::WrapperTypeInfo* Location::GetWrapperTypeInfo() const {
  return &bindings::V8Location::kWrapperTypeInfo;
}

void Location::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
}

// The URL of the window's document, as Blink's Location reads it from its
// window's document each time.
GURL Location::Url() const {
  if (!window_) {
    return GURL(url::kAboutBlankURL);
  }
  return GURL(window_->GetDocument().UrlForBinding());
}

std::u16string Location::Href() const {
  return base::UTF8ToUTF16(Url().spec());
}

std::u16string Location::Origin() const {
  return base::UTF8ToUTF16(url::Origin::Create(Url()).Serialize());
}

std::u16string Location::Protocol() const {
  return base::StrCat({base::UTF8ToUTF16(Url().scheme()), u":"});
}

std::u16string Location::Host() const {
  const GURL url = Url();
  if (!url.has_port()) {
    return base::UTF8ToUTF16(url.host());
  }
  return base::UTF8ToUTF16(base::StrCat({url.host(), ":", url.port()}));
}

std::u16string Location::Hostname() const {
  return base::UTF8ToUTF16(Url().host());
}

std::u16string Location::Port() const {
  return base::UTF8ToUTF16(Url().port());
}

std::u16string Location::Pathname() const {
  return base::UTF8ToUTF16(Url().path());
}

// "?" and the query, or nothing for none or an empty one, as Blink's
// DOMURLUtilsReadOnly::search() gives it.
std::u16string Location::Search() const {
  const GURL url = Url();
  if (url.query().empty()) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(base::StrCat({"?", url.query()}));
}

std::u16string Location::Hash() const {
  const GURL url = Url();
  if (url.ref().empty()) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(base::StrCat({"#", url.ref()}));
}

std::u16string Location::ToString() const {
  return base::UTF8ToUTF16(Url().spec());
}

namespace {

// The URL of the window's document as Blink's Location setters change it: the
// document's own, not the about:blank that the getters give a document of no
// URL.
ResolvedURL DocumentUrl(const Window* window) {
  return ResolvedURL(window ? window->GetDocument().UrlForBinding()
                            : std::u16string());
}

// What a fragment is once a setter has taken its "#", null and empty alike,
// as EqualIgnoringNullity() compares them.
std::u16string FragmentOf(const ResolvedURL& url) {
  std::u16string fragment = url.FragmentIdentifierWithLeadingNumberSign();
  if (!fragment.empty()) {
    fragment.erase(0, 1);
  }
  return fragment;
}

}  // namespace

void Location::SetLocation(const std::u16string& url,
                           bindings::ExceptionState& exception_state,
                           FrameLoadType type) {
  // A Location whose window's context has gone navigates nothing.
  if (!window_ || !window_->HasContext()) {
    return;
  }
  // The entered window's document, which is the window's own in a Page.
  Document& document = window_->GetDocument();
  const GURL completed = document.CompleteUrl(url);
  if (!completed.is_valid()) {
    // The string that KURL keeps of a URL that does not parse, which is what
    // the canonicalizer wrote of it, such as "http://[/" for "http://[".
    const ResolvedURL resolved(
        ResolvedURL(base::UTF8ToUTF16(document.BaseUrl().spec())), url);
    exception_state.ThrowDOMException(
        "SyntaxError",
        base::StrCat({"'", base::UTF16ToUTF8(resolved.GetString()),
                      "' is not a valid URL."}));
    return;
  }
  FrameLoadRequest request;
  request.url = completed;
  request.reason = ClientNavigationReason::kFrameNavigation;
  Navigate(*window_, request, type);
}

void Location::SetHref(const std::u16string& value,
                       bindings::ExceptionState& exception_state) {
  SetLocation(value, exception_state);
}

void Location::SetProtocol(const std::u16string& value,
                           bindings::ExceptionState& exception_state) {
  ResolvedURL url = DocumentUrl(window_);
  if (!url.SetProtocol(value)) {
    exception_state.ThrowDOMException(
        "SyntaxError", base::StrCat({"'", base::UTF16ToUTF8(value),
                                     "' is an invalid protocol."}));
    return;
  }
  SetLocation(url.GetString(), exception_state);
}

void Location::SetHost(const std::u16string& value,
                       bindings::ExceptionState& exception_state) {
  ResolvedURL url = DocumentUrl(window_);
  url.SetHostAndPort(value);
  SetLocation(url.GetString(), exception_state);
}

void Location::SetHostname(const std::u16string& value,
                           bindings::ExceptionState& exception_state) {
  ResolvedURL url = DocumentUrl(window_);
  url.SetHost(value);
  SetLocation(url.GetString(), exception_state);
}

void Location::SetPort(const std::u16string& value,
                       bindings::ExceptionState& exception_state) {
  ResolvedURL url = DocumentUrl(window_);
  url.SetPort(value);
  SetLocation(url.GetString(), exception_state);
}

void Location::SetPathname(const std::u16string& value,
                           bindings::ExceptionState& exception_state) {
  ResolvedURL url = DocumentUrl(window_);
  url.SetPath(value);
  SetLocation(url.GetString(), exception_state);
}

void Location::SetSearch(const std::u16string& value,
                         bindings::ExceptionState& exception_state) {
  ResolvedURL url = DocumentUrl(window_);
  url.SetQuery(value);
  SetLocation(url.GetString(), exception_state);
}

void Location::SetHash(const std::u16string& value,
                       bindings::ExceptionState& exception_state) {
  ResolvedURL url = DocumentUrl(window_);
  const std::u16string old_fragment = FragmentOf(url);
  url.SetFragmentIdentifier(value.starts_with(u'#') ? value.substr(1) : value);
  // Compared once the URL has canonicalized it: the same fragment navigates
  // nowhere.
  if (old_fragment == FragmentOf(url)) {
    return;
  }
  SetLocation(url.GetString(), exception_state);
}

void Location::Assign(const std::u16string& url,
                      bindings::ExceptionState& exception_state) {
  SetLocation(url, exception_state);
}

void Location::Replace(const std::u16string& url,
                       bindings::ExceptionState& exception_state) {
  SetLocation(url, exception_state, FrameLoadType::kReplaceCurrentItem);
}

// LocalFrame::Reload(): the document's URL again, validating what the cache
// holds, with no rate limit, and nothing for a javascript: URL.
void Location::Reload() {
  if (!window_ || !window_->HasContext()) {
    return;
  }
  const GURL url(window_->GetDocument().UrlForBinding());
  if (url.SchemeIs("javascript")) {
    return;
  }
  FrameLoadRequest request;
  request.url = url;
  request.reason = ClientNavigationReason::kReload;
  StartNavigation(*window_, request, FrameLoadType::kReload);
}

}  // namespace mitto
