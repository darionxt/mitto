// The pre element, and listing and xmp, which share its interface.
//
// Upstream: third_party/blink/renderer/core/html/html_pre_element.h
// Upstream: third_party/blink/renderer/core/html/html_pre_element.cc

#ifndef MITTO_DOM_HTML_PRE_ELEMENT_H_
#define MITTO_DOM_HTML_PRE_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLPreElement final : public HTMLElement {
 public:
  static HTMLPreElement* Create(Document& document,
                                const std::u16string& local_name);

  HTMLPreElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_PRE_ELEMENT_H_
