// The style element. The Runtime has no style yet (roadmap, M9).
//
// Upstream: third_party/blink/renderer/core/html/html_style_element.h
// Upstream: third_party/blink/renderer/core/html/html_style_element.cc

#ifndef MITTO_DOM_HTML_STYLE_ELEMENT_H_
#define MITTO_DOM_HTML_STYLE_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLStyleElement final : public HTMLElement {
 public:
  static HTMLStyleElement* Create(Document& document);

  explicit HTMLStyleElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  DOMTokenList* Blocking() {
    return EnsureTokenList(u"blocking",
                           DOMTokenList::SupportedTokens::kBlocking);
  }
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_STYLE_ELEMENT_H_
