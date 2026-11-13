// The base element, whose href gives its document's base URL.
//
// Upstream: third_party/blink/renderer/core/html/html_base_element.h
// Upstream: third_party/blink/renderer/core/html/html_base_element.cc

#ifndef MITTO_DOM_HTML_BASE_ELEMENT_H_
#define MITTO_DOM_HTML_BASE_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLBaseElement final : public HTMLElement {
 public:
  static HTMLBaseElement* Create(Document& document);

  explicit HTMLBaseElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface. href is resolved against the document's
  // fallback base URL, which a base element cannot change.
  std::u16string Href() const;
  void SetHref(const std::u16string& href);

  // Node's.
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void RemovedFrom(Node& insertion_point) override;
  // Element's.
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_BASE_ELEMENT_H_
