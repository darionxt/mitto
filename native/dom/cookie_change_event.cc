#include "mitto/dom/cookie_change_event.h"

#include <string_view>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_cookie_change_event.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

bindings::EventInit ChangeEventInit() {
  return bindings::EventInit();
}

std::u16string SameSiteName(PageCookieSameSite same_site) {
  switch (same_site) {
    case PageCookieSameSite::kStrict:
      return u"strict";
    case PageCookieSameSite::kLax:
      return u"lax";
    case PageCookieSameSite::kNoRestriction:
      return u"none";
    case PageCookieSameSite::kNone:
      return u"";
  }
}

}  // namespace

// static
CookieChangeEvent* CookieChangeEvent::Create(
    const std::u16string& type,
    const bindings::CookieChangeEventInit& init) {
  return bindings::MakeGarbageCollected<CookieChangeEvent>(type, init);
}

CookieChangeEvent::CookieChangeEvent(
    const std::u16string& type,
    const bindings::CookieChangeEventInit& init)
    : Event(type, init),
      changed_(init.changed.value_or(std::vector<bindings::CookieListItem>())),
      deleted_(init.deleted.value_or(std::vector<bindings::CookieListItem>())) {
}

CookieChangeEvent::CookieChangeEvent(
    std::vector<bindings::CookieListItem> changed,
    std::vector<bindings::CookieListItem> deleted)
    : Event(u"change", ChangeEventInit()),
      changed_(std::move(changed)),
      deleted_(std::move(deleted)) {}

CookieChangeEvent::~CookieChangeEvent() = default;

const bindings::WrapperTypeInfo* CookieChangeEvent::GetWrapperTypeInfo() const {
  return &bindings::V8CookieChangeEvent::kWrapperTypeInfo;
}

void CookieChangeEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(changed_array_);
  visitor->Trace(deleted_array_);
}

v8::Local<v8::Value> CookieChangeEvent::Changed() {
  return SameFrozenArray(changed_, changed_array_);
}

v8::Local<v8::Value> CookieChangeEvent::Deleted() {
  return SameFrozenArray(deleted_, deleted_array_);
}

// static
v8::Local<v8::Value> CookieChangeEvent::SameFrozenArray(
    const std::vector<bindings::CookieListItem>& items,
    v8::TracedReference<v8::Array>& array) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // [SameObject, SaveSameObject]: the wrapper keeps the array it gave.
  if (array.IsEmpty()) {
    std::vector<v8::Local<v8::Value>> objects;
    objects.reserve(items.size());
    for (const bindings::CookieListItem& item : items) {
      objects.push_back(ToV8Object(isolate, item));
    }
    array.Reset(isolate, FrozenArray(isolate, objects));
  }
  return array.Get(isolate);
}

// static
bindings::CookieListItem CookieChangeEvent::ToCookieListItem(
    const PageCookie& cookie,
    bool is_deleted) {
  bindings::CookieListItem item;
  // Blink reads the cookie's strings as UTF-8.
  item.name = base::UTF8ToUTF16(cookie.name);
  item.path = base::UTF8ToUTF16(cookie.path);
  item.secure = cookie.secure;
  if (cookie.same_site != PageCookieSameSite::kNone) {
    item.same_site = SameSiteName(cookie.same_site);
  }
  // The domain of a host-only cookie is null; that of a domain cookie has no
  // leading dot.
  if (cookie.domain.starts_with('.')) {
    item.domain = base::UTF8ToUTF16(std::string_view(cookie.domain).substr(1));
  } else {
    item.domain = std::optional<std::u16string>();
  }
  if (!is_deleted) {
    item.value = base::UTF8ToUTF16(cookie.value);
    // ConvertTimeToDOMHighResTimeStamp(): milliseconds since the Unix epoch,
    // with the microseconds as a fraction.
    item.expires = cookie.expires
                       ? std::optional<double>(
                             static_cast<double>(*cookie.expires) / 1000.0)
                       : std::optional<double>();
  }
  item.partitioned = cookie.partitioned;
  return item;
}

// static
v8::Local<v8::Object> CookieChangeEvent::ToV8Object(
    v8::Isolate* isolate,
    const bindings::CookieListItem& item) {
  std::vector<std::pair<std::string_view, v8::Local<v8::Value>>> members;
  if (item.domain) {
    members.emplace_back("domain",
                         bindings::ToV8NullableString(isolate, *item.domain));
  }
  if (item.expires) {
    members.emplace_back(
        "expires", *item.expires ? v8::Local<v8::Value>(
                                       v8::Number::New(isolate, **item.expires))
                                 : v8::Local<v8::Value>(v8::Null(isolate)));
  }
  if (item.name) {
    members.emplace_back("name", bindings::ToV8String(isolate, *item.name));
  }
  if (item.partitioned) {
    members.emplace_back("partitioned",
                         v8::Boolean::New(isolate, *item.partitioned));
  }
  if (item.path) {
    members.emplace_back("path", bindings::ToV8String(isolate, *item.path));
  }
  if (item.same_site) {
    members.emplace_back("sameSite",
                         bindings::ToV8String(isolate, *item.same_site));
  }
  if (item.secure) {
    members.emplace_back("secure", v8::Boolean::New(isolate, *item.secure));
  }
  if (item.value) {
    members.emplace_back("value", bindings::ToV8String(isolate, *item.value));
  }
  return PlainObject(isolate, members);
}

}  // namespace mitto
