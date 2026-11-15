#include "mitto/dom/url_utils.h"

#include <optional>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/resolved_url.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

std::u16string UrlUtils::Href() const {
  const ResolvedURL url = Url();
  if (url.IsNull()) {
    return Input();
  }
  return url.GetString();
}

std::u16string UrlUtils::Origin() const {
  const ResolvedURL url = Url();
  if (url.IsNull()) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(
      url::Origin::Create(GURL(url.GetString())).Serialize());
}

std::u16string UrlUtils::Protocol() const {
  return Url().Protocol() + u":";
}

void UrlUtils::SetProtocol(const std::u16string& value) {
  ResolvedURL url = Url();
  if (url.IsNull()) {
    return;
  }
  url.SetProtocol(value);
  SetUrl(url);
}

std::u16string UrlUtils::Username() const {
  return Url().User();
}

void UrlUtils::SetUsername(const std::u16string& value) {
  ResolvedURL url = Url();
  if (url.IsNull()) {
    return;
  }
  url.SetUser(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string UrlUtils::Password() const {
  return Url().Pass();
}

void UrlUtils::SetPassword(const std::u16string& value) {
  ResolvedURL url = Url();
  if (url.IsNull()) {
    return;
  }
  url.SetPass(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string UrlUtils::Host() const {
  const ResolvedURL url = Url();
  if (url.HostEnd() == url.PathStart() ||
      IsDefaultPortForProtocol(url.Port(), url.Protocol())) {
    return url.Host();
  }
  return url.Host() + u":" + base::NumberToString16(url.Port());
}

void UrlUtils::SetHost(const std::u16string& value) {
  ResolvedURL url = Url();
  if ((value.empty() && !url.CanRemoveHost()) || !url.CanSetHostOrPort()) {
    return;
  }
  url.SetHostAndPort(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string UrlUtils::Hostname() const {
  return Url().Host();
}

void UrlUtils::SetHostname(const std::u16string& value) {
  ResolvedURL url = Url();
  if ((value.empty() && !url.CanRemoveHost()) || !url.CanSetHostOrPort()) {
    return;
  }
  url.SetHost(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string UrlUtils::Port() const {
  const ResolvedURL url = Url();
  return url.HasPort() ? base::NumberToString16(url.Port()) : std::u16string();
}

void UrlUtils::SetPort(const std::u16string& value) {
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

std::u16string UrlUtils::Pathname() const {
  return Url().Path();
}

void UrlUtils::SetPathname(const std::u16string& value) {
  ResolvedURL url = Url();
  if (!url.CanSetPathname()) {
    return;
  }
  url.SetPath(value);
  if (url.IsValid()) {
    SetUrl(url);
  }
}

std::u16string UrlUtils::Search() const {
  return Url().QueryWithLeadingQuestionMark();
}

void UrlUtils::SetSearch(const std::u16string& value) {
  SetSearchInternal(value);
}

void UrlUtils::SetSearchInternal(const std::u16string& value) {
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

std::u16string UrlUtils::Hash() const {
  return Url().FragmentIdentifierWithLeadingNumberSign();
}

void UrlUtils::SetHash(const std::u16string& value) {
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

}  // namespace mitto
