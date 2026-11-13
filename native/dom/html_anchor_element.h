// The a element, a hyperlink. A Page never follows links, so what it has is the
// URL of its href.
//
// Upstream: third_party/blink/renderer/core/html/html_anchor_element.h
// Upstream: third_party/blink/renderer/core/html/html_anchor_element.cc

#ifndef MITTO_DOM_HTML_ANCHOR_ELEMENT_H_
#define MITTO_DOM_HTML_ANCHOR_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLAnchorElement final : public HTMLElement {
 public:
  static HTMLAnchorElement* Create(Document& document);

  explicit HTMLAnchorElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface. href, which toString() gives too, is the
  // href attribute resolved as a URL, as Blink's Href() resolves it.
  std::u16string Href() const;
  void SetHref(const std::u16string& value);
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ANCHOR_ELEMENT_H_
