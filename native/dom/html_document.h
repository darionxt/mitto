// The HTML Standard's HTMLDocument, the interface of a window's document and of
// the documents the HTML parser makes. It adds nothing to Document but a
// named getter for forms and images by name, which is a Known Gap.
//
// Upstream: third_party/blink/renderer/core/html/html_document.h
// Upstream: third_party/blink/renderer/core/html/html_document.cc

#ifndef MITTO_DOM_HTML_DOCUMENT_H_
#define MITTO_DOM_HTML_DOCUMENT_H_

#include "mitto/dom/document.h"

namespace mitto {

class HTMLDocument final : public Document {
 public:
  // An HTML document at about:blank.
  static HTMLDocument* Create();

  HTMLDocument();
  ~HTMLDocument() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_DOCUMENT_H_
