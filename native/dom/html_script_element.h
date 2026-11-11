// The script element. Documents run their scripts once a Page loads them
// (#183).
//
// Upstream: third_party/blink/renderer/core/html/html_script_element.h
// Upstream: third_party/blink/renderer/core/html/html_script_element.cc

#ifndef MITTO_DOM_HTML_SCRIPT_ELEMENT_H_
#define MITTO_DOM_HTML_SCRIPT_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLScriptElement final : public HTMLElement {
 public:
  static HTMLScriptElement* Create(Document& document);

  explicit HTMLScriptElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Text() const { return ChildTextContent(); }
  void SetText(const std::u16string& text,
               bindings::ExceptionState& exception_state);
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_SCRIPT_ELEMENT_H_
