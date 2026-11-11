// The meta element.
//
// Upstream: third_party/blink/renderer/core/html/html_meta_element.h
// Upstream: third_party/blink/renderer/core/html/html_meta_element.cc

#ifndef MITTO_DOM_HTML_META_ELEMENT_H_
#define MITTO_DOM_HTML_META_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLMetaElement final : public HTMLElement {
 public:
  static HTMLMetaElement* Create(Document& document);

  explicit HTMLMetaElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_META_ELEMENT_H_
