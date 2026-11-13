// The area element, a hyperlink, with the URL's parts of its href.
//
// Upstream: third_party/blink/renderer/core/html/html_area_element.h
// Upstream: third_party/blink/renderer/core/html/html_area_element.cc

#ifndef MITTO_DOM_HTML_AREA_ELEMENT_H_
#define MITTO_DOM_HTML_AREA_ELEMENT_H_

#include <string>

#include "mitto/dom/html_anchor_element.h"

namespace mitto {

class HTMLAreaElement final : public HTMLAnchorElementBase {
 public:
  static HTMLAreaElement* Create(Document& document);

  explicit HTMLAreaElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_AREA_ELEMENT_H_
