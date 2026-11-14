// The link element. It loads nothing yet (roadmap, M8).
//
// Upstream: third_party/blink/renderer/core/html/html_link_element.h
// Upstream: third_party/blink/renderer/core/html/html_link_element.cc

#ifndef MITTO_DOM_HTML_LINK_ELEMENT_H_
#define MITTO_DOM_HTML_LINK_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLLinkElement final : public HTMLElement {
 public:
  static HTMLLinkElement* Create(Document& document);

  explicit HTMLLinkElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface: its token lists, of the link types and
  // the blocking tokens that Blink supports; sizes supports none.
  DOMTokenList* RelList() {
    return EnsureTokenList(u"rel", DOMTokenList::SupportedTokens::kRel);
  }
  DOMTokenList* Sizes() {
    return EnsureTokenList(u"sizes", DOMTokenList::SupportedTokens::kNone);
  }
  DOMTokenList* Blocking() {
    return EnsureTokenList(u"blocking",
                           DOMTokenList::SupportedTokens::kBlocking);
  }
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_LINK_ELEMENT_H_
