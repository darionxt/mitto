#include "mitto/dom/document_type.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_document_type.h"

namespace mitto {

// static
DocumentType* DocumentType::Create(Document& document,
                                   const std::u16string& name,
                                   const std::u16string& public_id,
                                   const std::u16string& system_id) {
  return bindings::MakeGarbageCollected<DocumentType>(document, name, public_id,
                                                      system_id);
}

DocumentType::DocumentType(Document& document,
                           const std::u16string& name,
                           const std::u16string& public_id,
                           const std::u16string& system_id)
    : Node(&document),
      name_(name),
      public_id_(public_id),
      system_id_(system_id) {}

const bindings::WrapperTypeInfo* DocumentType::GetWrapperTypeInfo() const {
  return &bindings::V8DocumentType::kWrapperTypeInfo;
}

Node* DocumentType::CloneWithoutChildren(Document& document) const {
  return Create(document, name_, public_id_, system_id_);
}

uint16_t DocumentType::GetNodeType() const {
  return kDocumentTypeNode;
}

std::u16string DocumentType::NodeName() const {
  return name_;
}

}  // namespace mitto
