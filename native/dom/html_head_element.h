// The head element.
//
// Upstream: third_party/blink/renderer/core/html/html_head_element.h
// Upstream: third_party/blink/renderer/core/html/html_head_element.cc

#ifndef MITTO_DOM_HTML_HEAD_ELEMENT_H_
#define MITTO_DOM_HTML_HEAD_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLHeadElement final : public HTMLElement {
 public:
  static HTMLHeadElement* Create(Document& document);

  explicit HTMLHeadElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_HEAD_ELEMENT_H_
