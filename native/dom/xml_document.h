// The DOM's XMLDocument, the interface of the documents that
// DOMImplementation's createDocument() makes. It adds nothing to Document;
// its MIME type follows the namespace of the element it was made for.
//
// Upstream: third_party/blink/renderer/core/dom/xml_document.h
// Upstream: third_party/blink/renderer/core/dom/xml_document.cc

#ifndef MITTO_DOM_XML_DOCUMENT_H_
#define MITTO_DOM_XML_DOCUMENT_H_

#include <string>

#include "mitto/dom/document.h"

namespace mitto {

class XMLDocument final : public Document {
 public:
  // An XML document whose contentType is `content_type`, such as
  // "application/xhtml+xml", as Blink's XMLDocument::CreateXHTML() makes one,
  // or "image/svg+xml", as CreateSVG() does.
  static XMLDocument* Create(std::u16string content_type);

  explicit XMLDocument(std::u16string content_type);
  ~XMLDocument() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

 protected:
  // Document's: a copy is an XMLDocument too.
  Document* CloneDocumentWithoutChildren() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_DOCUMENT_H_
