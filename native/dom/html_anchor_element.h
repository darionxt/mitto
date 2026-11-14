// The a element, a hyperlink, and the base of it and the area element, which
// share the URL's parts of their href. A Page never follows links.
//
// Upstream: third_party/blink/renderer/core/html/html_anchor_element.h
// Upstream: third_party/blink/renderer/core/html/html_anchor_element.cc
// Upstream: third_party/blink/renderer/core/url/url_utils.cc
// Upstream: third_party/blink/renderer/core/url/url_utils_read_only.cc

#ifndef MITTO_DOM_HTML_ANCHOR_ELEMENT_H_
#define MITTO_DOM_HTML_ANCHOR_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class ResolvedURL;

// Blink's HTMLAnchorElementBase with its UrlUtils: each part of the URL that
// href resolves to, and each setter, which changes that part and writes the
// URL back into href, as Blink's do.
class HTMLAnchorElementBase : public HTMLElement {
 public:
  // The members of HTMLHyperlinkElementUtils.
  std::u16string Href() const;
  void SetHref(const std::u16string& value);
  std::u16string Origin() const;
  std::u16string Protocol() const;
  void SetProtocol(const std::u16string& value);
  std::u16string Username() const;
  void SetUsername(const std::u16string& value);
  std::u16string Password() const;
  void SetPassword(const std::u16string& value);
  std::u16string Host() const;
  void SetHost(const std::u16string& value);
  std::u16string Hostname() const;
  void SetHostname(const std::u16string& value);
  std::u16string Port() const;
  void SetPort(const std::u16string& value);
  std::u16string Pathname() const;
  void SetPathname(const std::u16string& value);
  std::u16string Search() const;
  void SetSearch(const std::u16string& value);
  std::u16string Hash() const;
  void SetHash(const std::u16string& value);

  // HTMLElement's: a link with href is draggable by default, and focusable.
  bool Draggable() const override;
  int32_t DefaultTabIndex() const override;

 protected:
  HTMLAnchorElementBase(Document& document, const std::u16string& local_name);

 private:
  // The URL that href resolves to, null without href or for one that does
  // not resolve, Blink's Url().
  ResolvedURL Url() const;
  // Writes `url` into href, Blink's SetUrl().
  void SetUrl(const ResolvedURL& url);
};

class HTMLAnchorElement final : public HTMLAnchorElementBase {
 public:
  static HTMLAnchorElement* Create(Document& document);

  explicit HTMLAnchorElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ANCHOR_ELEMENT_H_
