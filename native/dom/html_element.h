// The DOM's HTMLElement: an element of the HTML namespace, and the class of
// those whose name has no interface of its own, such as section or b.
//
// Upstream: third_party/blink/renderer/core/html/html_element.h
// Upstream: third_party/blink/renderer/core/html/html_element.cc

#ifndef MITTO_DOM_HTML_ELEMENT_H_
#define MITTO_DOM_HTML_ELEMENT_H_

#include <string>

#include "mitto/dom/element.h"

namespace mitto {

class HTMLElement : public Element {
 public:
  static HTMLElement* Create(Document& document,
                             const std::u16string& local_name);

  HTMLElement(Document& document, const std::u16string& local_name);
  ~HTMLElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface beyond its reflected attributes.
  std::u16string Autocapitalize() const;
  void SetAutocapitalize(const std::u16string& value);
  // Fires a click at the element, which runs its activation behaviour, as
  // Blink's HTMLElement::click() does for a script.
  void Click() { DispatchSimulatedClick(); }
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ELEMENT_H_
