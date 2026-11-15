// The ol element.
//
// Upstream: third_party/blink/renderer/core/html/html_olist_element.h
// Upstream: third_party/blink/renderer/core/html/html_olist_element.cc

#ifndef MITTO_DOM_HTML_OLIST_ELEMENT_H_
#define MITTO_DOM_HTML_OLIST_ELEMENT_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLOListElement final : public HTMLElement {
 public:
  static HTMLOListElement* Create(Document& document);

  explicit HTMLOListElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface beyond its reflected attributes: the start
  // attribute's integer, else 1, which reversed leaves alone, as Blink's
  // start() gives it without layout.
  int32_t Start() const;
  void SetStart(int32_t start);
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_OLIST_ELEMENT_H_
