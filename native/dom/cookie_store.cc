#include "mitto/dom/cookie_store.h"

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_cookie_store.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/cookie_change_event.h"
#include "mitto/dom/cookie_jar.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/base/url_util.h"
#include "url/url_canon.h"
#include "url/url_util.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

constexpr char kAccessDeniedMessage[] =
    "Access to the CookieStore API is denied in this context.";

// StripHttpWhitespace(), with CookieStoreAPIWhitespaceStripping, which is on
// for stable: spaces and tabs at either end.
std::u16string StripHttpWhitespace(const std::u16string& string) {
  const size_t first = string.find_first_not_of(u" \t");
  if (first == std::u16string::npos) {
    return std::u16string();
  }
  const size_t last = string.find_last_not_of(u" \t");
  return string.substr(first, last - first + 1);
}

// network::IsUrlPotentiallyTrustworthy() for a document's URL: a secure
// scheme, or a host that is the machine itself.
bool IsUrlPotentiallyTrustworthy(const GURL& url) {
  if (url.SchemeIs(url::kHttpsScheme) || url.SchemeIs(url::kWssScheme)) {
    return true;
  }
  return (url.SchemeIs(url::kHttpScheme) || url.SchemeIs(url::kWsScheme)) &&
         net::IsLocalhost(url);
}

bool IsLocalScheme(std::string_view scheme) {
  for (const std::string& local : url::GetLocalSchemes()) {
    if (local == scheme) {
      return true;
    }
  }
  return false;
}

// net::cookie_util::GetEffectiveDomain().
std::string GetEffectiveDomain(std::string_view scheme, std::string_view host) {
  if (scheme == "http" || scheme == "https" || scheme == "ws" ||
      scheme == "wss") {
    return net::registry_controlled_domains::GetDomainAndRegistry(
        host, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  }
  return std::string(host.starts_with('.') ? host.substr(1) : host);
}

// Runs `run` in `context`, as a task of the Page settles a promise of it,
// unless the window has let the context go.
template <typename Run>
void RunInContext(v8::Isolate* isolate,
                  const v8::Global<v8::Context>& context_handle,
                  Run run) {
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = context_handle.Get(isolate);
  const Window* const window =
      bindings::V8Window::ToWrappable(isolate, context->Global());
  if (!window || !window->HasContext()) {
    return;
  }
  const v8::Context::Scope context_scope(context);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  run(context);
}

v8::Local<v8::Value> CookieToV8(v8::Isolate* isolate,
                                const PageCookie& cookie) {
  return CookieChangeEvent::ToV8Object(
      isolate, CookieChangeEvent::ToCookieListItem(cookie, false));
}

}  // namespace

CookieStore::CookieStore(Window* window) : window_(window) {}

CookieStore::~CookieStore() = default;

const bindings::WrapperTypeInfo* CookieStore::GetWrapperTypeInfo() const {
  return &bindings::V8CookieStore::kWrapperTypeInfo;
}

void CookieStore::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(window_);
}

// static
bool CookieStore::IsCookieDomainValid(const GURL& url,
                                      std::string_view domain_string) {
  // Non-ASCII domains only warn while CookieDomainRejectNonASCII is off.
  const std::string_view url_host = url.host();
  // A host that ends in an empty label is not valid.
  if (url_host.ends_with("..")) {
    return false;
  }
  const bool is_host_ip = url.HostIsIPAddress();
  const bool domain_matches_host =
      base::EqualsCaseInsensitiveASCII(url_host, domain_string) ||
      (domain_string.starts_with('.') &&
       base::EqualsCaseInsensitiveASCII(url_host, domain_string.substr(1)));
  // An IP address takes a domain only of itself, as a host cookie.
  if (domain_string.empty() || (is_host_ip && domain_matches_host)) {
    return true;
  }
  if (is_host_ip) {
    return false;
  }
  if (domain_string.find('%') != std::string_view::npos) {
    return false;
  }
  url::CanonHostInfo ignored;
  std::string cookie_domain = net::CanonicalizeHost(domain_string, &ignored);
  if (cookie_domain.empty()) {
    return false;
  }
  if (cookie_domain[0] != '.') {
    cookie_domain = "." + cookie_domain;
  }
  // The URL and the domain must share their domain and registry.
  const std::string url_domain_and_registry =
      GetEffectiveDomain(url.scheme(), url_host);
  if (url_domain_and_registry.empty()) {
    // An exact match of the host is a host cookie, but an IP address, an
    // intranet host or a public suffix takes no domain cookie.
    const std::string normalized_domain = base::ToLowerASCII(
        domain_string[0] == '.' ? domain_string.substr(1) : domain_string);
    return url_host == normalized_domain;
  }
  if (url_domain_and_registry !=
      GetEffectiveDomain(url.scheme(), cookie_domain)) {
    return false;
  }
  // The host is the domain or one of its subdomains.
  const bool not_a_subdomain =
      (url_host.length() < cookie_domain.length())
          ? !cookie_domain.starts_with('.') ||
                std::string_view(cookie_domain).substr(1) != url_host
          : url_host.compare(url_host.length() - cookie_domain.length(),
                             cookie_domain.length(), cookie_domain) != 0;
  return !not_a_subdomain;
}

CookieJar* CookieStore::JarForAccess(
    bindings::ExceptionState& exception_state) const {
  // SecurityOrigin::CanAccessCookies(): a document that has no jar has an
  // opaque origin.
  Window* const window = window_.Get();
  CookieJar* const jar =
      window ? window->GetDocument().GetCookieJar() : nullptr;
  if (!jar) {
    exception_state.ThrowDOMException("SecurityError", kAccessDeniedMessage);
  }
  return jar;
}

v8::Local<v8::Promise> CookieStore::Get(
    const std::u16string& name,
    bindings::ExceptionState& exception_state) {
  bindings::CookieStoreGetOptions options;
  options.name = name;
  return Get(options, exception_state);
}

v8::Local<v8::Promise> CookieStore::Get(
    const bindings::CookieStoreGetOptions& options,
    bindings::ExceptionState& exception_state) {
  if (!options.name && !options.url) {
    exception_state.ThrowTypeError("CookieStoreGetOptions must not be empty");
    return {};
  }
  return DoRead(options, /*get_all=*/false, exception_state);
}

v8::Local<v8::Promise> CookieStore::GetAll(
    const std::u16string& name,
    bindings::ExceptionState& exception_state) {
  bindings::CookieStoreGetOptions options;
  options.name = name;
  return GetAll(options, exception_state);
}

v8::Local<v8::Promise> CookieStore::GetAll(
    const bindings::CookieStoreGetOptions& options,
    bindings::ExceptionState& exception_state) {
  return DoRead(options, /*get_all=*/true, exception_state);
}

v8::Local<v8::Promise> CookieStore::Set(
    const std::u16string& name,
    const std::u16string& value,
    bindings::ExceptionState& exception_state) {
  bindings::CookieInit options;
  options.name = name;
  options.value = value;
  return Set(options, exception_state);
}

v8::Local<v8::Promise> CookieStore::Set(
    const bindings::CookieInit& options,
    bindings::ExceptionState& exception_state) {
  return DoWrite(options, exception_state);
}

// CookieStore::Delete(): a cookie that expired at the Unix epoch, which takes
// the place of the one of its name, domain and path.
v8::Local<v8::Promise> CookieStore::Delete(
    const std::u16string& name,
    bindings::ExceptionState& exception_state) {
  const std::u16string stripped_name = StripHttpWhitespace(name);
  bindings::CookieInit options;
  options.name = stripped_name;
  options.value = stripped_name.empty() ? u"deleted" : u"";
  options.expires = 0;
  return DoWrite(options, exception_state);
}

v8::Local<v8::Promise> CookieStore::Delete(
    const bindings::CookieStoreDeleteOptions& options,
    bindings::ExceptionState& exception_state) {
  const std::u16string stripped_name =
      StripHttpWhitespace(options.name.value_or(u""));
  bindings::CookieInit init;
  init.name = stripped_name;
  init.value = stripped_name.empty() ? u"deleted" : u"";
  init.expires = 0;
  init.domain = options.domain;
  init.path = options.path;
  init.same_site = u"strict";
  init.partitioned = options.partitioned;
  return DoWrite(init, exception_state);
}

// CookieStore::DoRead(), ToBackendOptions() and CookieUrlForRead().
v8::Local<v8::Promise> CookieStore::DoRead(
    const bindings::CookieStoreGetOptions& options,
    bool get_all,
    bindings::ExceptionState& exception_state) {
  CookieJar* const jar = JarForAccess(exception_state);
  if (!jar) {
    return {};
  }
  PageCookieStoreGet get;
  if (options.name) {
    get.name = base::UTF16ToUTF8(StripHttpWhitespace(*options.name));
  } else {
    // No name: every cookie, as a match of names that start with "".
    get.all = true;
  }
  if (options.url) {
    const GURL document_url(
        base::UTF16ToUTF8(window_->GetDocument().UrlForBinding()));
    GURL::Replacements without_fragment;
    without_fragment.ClearRef();
    const GURL cookie_url =
        document_url.Resolve(base::UTF16ToUTF8(*options.url))
            .ReplaceComponents(without_fragment);
    if (cookie_url.spec() != document_url.spec()) {
      exception_state.ThrowTypeError("URL must match the document URL");
      return {};
    }
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  jar->GetForCookieStore(
      std::move(get),
      base::BindOnce(
          [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
             v8::Global<v8::Promise::Resolver> resolver_handle, bool get_all,
             std::vector<PageCookie> cookies) {
            RunInContext(
                isolate, context_handle, [&](v8::Local<v8::Context> context) {
                  v8::Local<v8::Value> result;
                  if (get_all) {
                    std::vector<v8::Local<v8::Value>> items;
                    items.reserve(cookies.size());
                    for (const PageCookie& cookie : cookies) {
                      items.push_back(CookieToV8(isolate, cookie));
                    }
                    result =
                        v8::Array::New(isolate, items.data(), items.size());
                  } else if (cookies.empty()) {
                    result = v8::Null(isolate);
                  } else {
                    result = CookieToV8(isolate, cookies.front());
                  }
                  std::ignore =
                      resolver_handle.Get(isolate)->Resolve(context, result);
                });
          },
          isolate, v8::Global<v8::Context>(isolate, context),
          v8::Global<v8::Promise::Resolver>(isolate, resolver), get_all));
  return resolver->GetPromise();
}

// CookieStore::DoWrite() and OnSetCanonicalCookieResult().
v8::Local<v8::Promise> CookieStore::DoWrite(
    const bindings::CookieInit& options,
    bindings::ExceptionState& exception_state) {
  CookieJar* const jar = JarForAccess(exception_state);
  if (!jar) {
    return {};
  }
  std::optional<PageCookieStoreSet> set =
      ToCookieParams(options, exception_state);
  if (!set) {
    return {};
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  jar->SetForCookieStore(
      *std::move(set),
      base::BindOnce(
          [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
             v8::Global<v8::Promise::Resolver> resolver_handle, bool stored) {
            RunInContext(
                isolate, context_handle, [&](v8::Local<v8::Context> context) {
                  const v8::Local<v8::Promise::Resolver> resolver =
                      resolver_handle.Get(isolate);
                  if (stored) {
                    std::ignore =
                        resolver->Resolve(context, v8::Undefined(isolate));
                    return;
                  }
                  std::ignore = resolver->Reject(
                      context,
                      v8::Exception::TypeError(bindings::ToV8String(
                          isolate,
                          u"Cookie was malformed and could not be stored, "
                          u"due to problem(s) while parsing.")));
                });
          },
          isolate, v8::Global<v8::Context>(isolate, context),
          v8::Global<v8::Promise::Resolver>(isolate, resolver)));
  return resolver->GetPromise();
}

std::optional<PageCookieStoreSet> CookieStore::ToCookieParams(
    const bindings::CookieInit& options,
    bindings::ExceptionState& exception_state) const {
  const std::u16string name = StripHttpWhitespace(options.name.value_or(u""));
  const std::u16string value = StripHttpWhitespace(options.value.value_or(u""));
  if (name.empty() && value.find(u'=') != std::u16string::npos) {
    exception_state.ThrowTypeError(
        "Cookie value cannot contain '=' if the name is empty");
    return std::nullopt;
  }
  if (name.empty() && value.empty()) {
    exception_state.ThrowTypeError(
        "Cookie name and value both cannot be empty");
    return std::nullopt;
  }
  if (name.find(u'=') != std::u16string::npos) {
    exception_state.ThrowTypeError("Cookie name cannot contain '='");
    return std::nullopt;
  }
  base::Time expiry_time;
  // CookieStoreAPIMaxAge is on for stable. A null maxAge counts as none.
  if (options.max_age && *options.max_age) {
    if (options.expires) {
      exception_state.ThrowTypeError(
          "Cookie expires and maxAge cannot both be specified");
      return std::nullopt;
    }
    const int64_t max_age = **options.max_age;
    expiry_time = max_age <= 0 ? base::Time::Min()
                               : base::Time::Now() + base::Seconds(max_age);
  } else if (options.expires) {
    expiry_time = base::Time::FromMillisecondsSinceUnixEpoch(*options.expires);
  }
  const bool is_http_prefix =
      base::StartsWith(name, u"__http-", base::CompareCase::INSENSITIVE_ASCII);
  const bool is_host_http_prefix = base::StartsWith(
      name, u"__host-http-", base::CompareCase::INSENSITIVE_ASCII);
  if (is_http_prefix || is_host_http_prefix) {
    exception_state.ThrowTypeError(
        is_http_prefix ? "Cookies with \"__Http-\" prefix cannot be set using "
                         "the CookieStore API."
                       : "Cookies with \"__Host-Http-\" prefix cannot be set "
                         "using the CookieStore API.");
    return std::nullopt;
  }
  const bool is_host_prefixed_cookie =
      base::StartsWith(name, u"__host-", base::CompareCase::INSENSITIVE_ASCII);
  const GURL cookie_url(
      base::UTF16ToUTF8(window_->GetDocument().UrlForBinding()));
  PageCookieStoreSet set;
  if (options.domain) {
    if (is_host_prefixed_cookie) {
      exception_state.ThrowTypeError(
          "Cookies with \"__Host-\" prefix cannot have a domain");
      return std::nullopt;
    }
    // The leading dot that a Set-Cookie header may have, this API refuses.
    if (options.domain->starts_with(u'.')) {
      exception_state.ThrowTypeError("Cookie domain cannot start with \".\"");
      return std::nullopt;
    }
    set.domain = base::UTF16ToUTF8(base::ToLowerASCII(u"." + *options.domain));
    if (!IsCookieDomainValid(cookie_url, set.domain)) {
      exception_state.ThrowTypeError(
          "Cookie domain must domain-match current host");
      return std::nullopt;
    }
  }
  std::u16string path = options.path;
  if (!path.empty()) {
    if (is_host_prefixed_cookie && path != u"/") {
      exception_state.ThrowTypeError(
          "Cookies with \"__Host-\" prefix cannot have a non-\"/\" path");
      return std::nullopt;
    }
    if (!path.starts_with(u'/')) {
      exception_state.ThrowTypeError("Cookie path must start with \"/\"");
      return std::nullopt;
    }
    if (!path.ends_with(u'/')) {
      path += u'/';
    }
  }
  // The API sets only secure cookies, so not on an insecure origin, nor from
  // a local scheme, as document.cookie does not.
  if (!IsUrlPotentiallyTrustworthy(cookie_url) ||
      IsLocalScheme(cookie_url.scheme())) {
    exception_state.ThrowTypeError(
        "Cannot modify a secure cookie on insecure origin");
    return std::nullopt;
  }
  set.name = base::UTF16ToUTF8(name);
  set.value = base::UTF16ToUTF8(value);
  set.path = base::UTF16ToUTF8(path);
  set.expiry = expiry_time.ToDeltaSinceWindowsEpoch().InMicroseconds();
  set.same_site = options.same_site == u"lax" ? PageCookieSameSite::kLax
                  : options.same_site == u"none"
                      ? PageCookieSameSite::kNoRestriction
                      : PageCookieSameSite::kStrict;
  set.partitioned = options.partitioned;
  return set;
}

// AddedEventListener() and RemovedEventListener(): the store hears of changes
// while a page listens for them.
void CookieStore::ListenersChanged(std::u16string_view type) {
  if (HasEventListeners(u"change")) {
    StartObserving();
  } else {
    StopObserving();
  }
}

void CookieStore::StartObserving() {
  Window* const window = window_.Get();
  if (observing_ || !window || !window->HasContext()) {
    return;
  }
  // Without access to cookies, the store does not ask.
  CookieJar* const jar = window->GetDocument().GetCookieJar();
  if (!jar) {
    return;
  }
  observing_ = true;
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto context = std::make_shared<v8::Global<v8::Context>>(
      isolate, isolate->GetCurrentContext());
  jar->ListenForCookieStore(base::BindRepeating(
      [](cppgc::WeakPersistent<CookieStore> store, v8::Isolate* isolate,
         const std::shared_ptr<v8::Global<v8::Context>>& context,
         const PageCookieStoreChange& change) {
        if (!store) {
          return;
        }
        RunInContext(isolate, *context, [&](v8::Local<v8::Context>) {
          store->OnCookieChange(change);
        });
      },
      cppgc::WeakPersistent<CookieStore>(this), isolate, std::move(context)));
  window->AddContextDestroyedCallback(base::BindOnce(
      [](cppgc::WeakPersistent<CookieStore> store) {
        if (store) {
          store->StopObserving();
        }
      },
      cppgc::WeakPersistent<CookieStore>(this)));
}

void CookieStore::StopObserving() {
  if (!observing_) {
    return;
  }
  observing_ = false;
  Window* const window = window_.Get();
  if (CookieJar* const jar =
          window ? window->GetDocument().GetCookieJar() : nullptr) {
    jar->ListenForCookieStore(CookieJar::ChangeListener());
  }
}

// CookieStore::OnCookieChange() and CookieChangeEvent::ToEventInfo().
void CookieStore::OnCookieChange(const PageCookieStoreChange& change) {
  std::vector<bindings::CookieListItem> changed;
  std::vector<bindings::CookieListItem> deleted;
  (change.deleted ? deleted : changed)
      .push_back(
          CookieChangeEvent::ToCookieListItem(change.cookie, change.deleted));
  CookieChangeEvent* const event =
      bindings::MakeGarbageCollected<CookieChangeEvent>(std::move(changed),
                                                        std::move(deleted));
  event->SetTrusted(true);
  DispatchEvent(event);
}

}  // namespace mitto
