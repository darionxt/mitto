#include "mitto/dom/html_anchor_element.h"

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_anchor_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/resolved_url.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

HTMLAnchorElementBase::HTMLAnchorElementBase(Document& document,
                                             const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

ResolvedURL HTMLAnchorElementBase::Url() const {
  const std::u16string* href = FindAttribute(u"href");
  if (!href) {
    return ResolvedURL();
  }
  // Blink's Href() resolves it with the document's CompleteURL(), which
  // encodes the query in the document's encoding. A URL that does not resolve
  // is none, as Blink's Url() has it.
  const GURL url = GetDocument().CompleteUrl(StripHTMLSpaces(*href));
  if (!url.is_valid()) {
    return ResolvedURL();
  }
  return ResolvedURL(base::UTF8ToUTF16(url.spec()));
}

void HTMLAnchorElementBase::SetUrl(const ResolvedURL& url) {
  SetAttributeWithoutValidation(u"href", url.GetString());
}

std::u16string HTMLAnchorElementBase::Href() const {
  // UrlUtilsReadOnly::href(): the input itself for a null URL.
  const ResolvedURL url = Url();
  if (url.IsNull()) {
    const std::u16string* href = FindAttribute(u"href");
    return href ? *href : std::u16string();
  }
  return url.GetString();
}

void HTMLAnchorElementBase::SetHref(const std::u16string& value) {
  SetAttributeWithoutValidation(u"href", value);
}

std::u16string HTMLAnchorElementBase::Origin() const {
  const ResolvedURL url = Url();
  if (url.IsNull()) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(
      url::Origin::Create(GURL(url.GetString())).Serialize());
}

std::u16string HTMLAnchorElementBase::Protocol() const {
  return Url().Protocol() + u":";
}

void HTMLAnchorElementBase::SetProtocol(const std::u16string& value) {
  ResolvedURL url = Url();
  if (url.IsNull()) {
    return;
  }
  url.SetProtocol(value);
  SetUrl(url);
}

std::u16string HTMLAnchorElementBase::Username() const {
  return Url().User();
}

void HTMLAnchorElementBase::SetUsername(const std::u16string& value) {
  ResolvedURL url = Url();
  if (url.IsNull()) {
    return;
  }
  url.SetUser(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string HTMLAnchorElementBase::Password() const {
  return Url().Pass();
}

void HTMLAnchorElementBase::SetPassword(const std::u16string& value) {
  ResolvedURL url = Url();
  if (url.IsNull()) {
    return;
  }
  url.SetPass(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string HTMLAnchorElementBase::Host() const {
  const ResolvedURL url = Url();
  if (url.HostEnd() == url.PathStart() ||
      IsDefaultPortForProtocol(url.Port(), url.Protocol())) {
    return url.Host();
  }
  return url.Host() + u":" + base::NumberToString16(url.Port());
}

void HTMLAnchorElementBase::SetHost(const std::u16string& value) {
  ResolvedURL url = Url();
  if ((value.empty() && !url.CanRemoveHost()) || !url.CanSetHostOrPort()) {
    return;
  }
  url.SetHostAndPort(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string HTMLAnchorElementBase::Hostname() const {
  return Url().Host();
}

void HTMLAnchorElementBase::SetHostname(const std::u16string& value) {
  ResolvedURL url = Url();
  if ((value.empty() && !url.CanRemoveHost()) || !url.CanSetHostOrPort()) {
    return;
  }
  url.SetHost(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string HTMLAnchorElementBase::Port() const {
  const ResolvedURL url = Url();
  return url.HasPort() ? base::NumberToString16(url.Port()) : std::u16string();
}

void HTMLAnchorElementBase::SetPort(const std::u16string& value) {
  ResolvedURL url = Url();
  if (!url.CanSetHostOrPort()) {
    return;
  }
  if (!value.empty()) {
    url.SetPort(value);
  } else {
    url.RemovePort();
  }
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string HTMLAnchorElementBase::Pathname() const {
  return Url().Path();
}

void HTMLAnchorElementBase::SetPathname(const std::u16string& value) {
  ResolvedURL url = Url();
  if (!url.CanSetPathname()) {
    return;
  }
  url.SetPath(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string HTMLAnchorElementBase::Search() const {
  return Url().QueryWithLeadingQuestionMark();
}

void HTMLAnchorElementBase::SetSearch(const std::u16string& value) {
  ResolvedURL url = Url();
  if (!url.IsValid()) {
    return;
  }
  if (value.empty() || value == u"?") {
    url.SetQuery(std::nullopt);
  } else {
    url.SetQuery(value);
  }
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string HTMLAnchorElementBase::Hash() const {
  return Url().FragmentIdentifierWithLeadingNumberSign();
}

void HTMLAnchorElementBase::SetHash(const std::u16string& value) {
  ResolvedURL url = Url();
  if (url.IsNull()) {
    return;
  }
  if (!value.empty() && value[0] == '#') {
    url.SetFragmentIdentifier(value.substr(1));
  } else if (value.empty()) {
    url.RemoveFragmentIdentifier();
  } else {
    url.SetFragmentIdentifier(value);
  }
  if (url.IsValid()) {
    SetUrl(url);
  }
}

bool HTMLAnchorElementBase::Draggable() const {
  const std::u16string* value = FindAttribute(u"draggable");
  if (value && base::EqualsCaseInsensitiveASCII(*value, u"true")) {
    return true;
  }
  if (value && base::EqualsCaseInsensitiveASCII(*value, u"false")) {
    return false;
  }
  return FindAttribute(u"href");
}

int32_t HTMLAnchorElementBase::DefaultTabIndex() const {
  return 0;
}

bool HTMLAnchorElementBase::HasActivationBehavior() const {
  return FindAttribute(u"href");
}

// static
HTMLAnchorElement* HTMLAnchorElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLAnchorElement>(document);
}

HTMLAnchorElement::HTMLAnchorElement(Document& document)
    : HTMLAnchorElementBase(document, u"a") {}

const bindings::WrapperTypeInfo* HTMLAnchorElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLAnchorElement::kWrapperTypeInfo;
}

}  // namespace mitto
