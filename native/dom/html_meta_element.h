// The meta element, whose http-equiv="refresh" asks its document for a
// refresh once it is in the document, and again whenever its content or
// http-equiv changes there, as Blink's HttpEquiv::Process() takes it. Its other
// pragmas do nothing yet.
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

  // Node's and Element's:
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void DidNotifySubtreeInsertionsToDocument() override;
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

 private:
  // ProcessHttpEquiv().
  void ProcessHttpEquiv();
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_META_ELEMENT_H_
