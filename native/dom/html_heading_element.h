// The h1 to h6 elements.
//
// Upstream: third_party/blink/renderer/core/html/html_heading_element.h
// Upstream: third_party/blink/renderer/core/html/html_heading_element.cc

#ifndef MITTO_DOM_HTML_HEADING_ELEMENT_H_
#define MITTO_DOM_HTML_HEADING_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLHeadingElement final : public HTMLElement {
 public:
  static HTMLHeadingElement* Create(Document& document,
                                    const std::u16string& local_name);

  HTMLHeadingElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_HEADING_ELEMENT_H_
