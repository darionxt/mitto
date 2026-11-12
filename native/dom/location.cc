#include "mitto/dom/location.h"

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_location.h"
#include "mitto/dom/document.h"
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

}  // namespace mitto
