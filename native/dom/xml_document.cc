#include "mitto/dom/xml_document.h"

#include <utility>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_xml_document.h"

namespace mitto {

// static
XMLDocument* XMLDocument::Create(std::u16string content_type) {
  return bindings::MakeGarbageCollected<XMLDocument>(std::move(content_type));
}

XMLDocument::XMLDocument(std::u16string content_type)
    : Document(/*is_html=*/false) {
  SetContentType(std::move(content_type));
}

XMLDocument::~XMLDocument() = default;

const bindings::WrapperTypeInfo* XMLDocument::GetWrapperTypeInfo() const {
  return &bindings::V8XMLDocument::kWrapperTypeInfo;
}

Document* XMLDocument::CloneDocumentWithoutChildren() const {
  // The copy's MIME type is the original's, which Document copies.
  return Create(u"application/xml");
}

}  // namespace mitto
