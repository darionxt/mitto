// The DOM's DocumentType, a document's doctype.
//
// Upstream: third_party/blink/renderer/core/dom/document_type.h
// Upstream: third_party/blink/renderer/core/dom/document_type.cc

#ifndef MITTO_DOM_DOCUMENT_TYPE_H_
#define MITTO_DOM_DOCUMENT_TYPE_H_

#include <string>

#include "mitto/dom/node.h"

namespace mitto {

class DocumentType final : public Node {
 public:
  static DocumentType* Create(Document& document,
                              const std::u16string& name,
                              const std::u16string& public_id,
                              const std::u16string& system_id);

  DocumentType(Document& document,
               const std::u16string& name,
               const std::u16string& public_id,
               const std::u16string& system_id);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Name() const { return name_; }
  const std::u16string& PublicId() const { return public_id_; }
  const std::u16string& SystemId() const { return system_id_; }

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

 protected:
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  const std::u16string name_;
  const std::u16string public_id_;
  const std::u16string system_id_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_TYPE_H_
