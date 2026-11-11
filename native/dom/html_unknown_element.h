// An HTML element whose name the HTML Standard does not know.
//
// Upstream: third_party/blink/renderer/core/html/html_unknown_element.h
// Upstream: third_party/blink/renderer/core/html/html_unknown_element.cc

#ifndef MITTO_DOM_HTML_UNKNOWN_ELEMENT_H_
#define MITTO_DOM_HTML_UNKNOWN_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLUnknownElement final : public HTMLElement {
 public:
  static HTMLUnknownElement* Create(Document& document,
                                    const std::u16string& local_name);

  HTMLUnknownElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_UNKNOWN_ELEMENT_H_
