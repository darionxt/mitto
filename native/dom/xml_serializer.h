// DOM Parsing's XMLSerializer: the markup of a node as XML, whatever its
// document, as Blink writes it (markup_serializer.h).
//
// Upstream: third_party/blink/renderer/core/xml/xml_serializer.h
// Upstream: third_party/blink/renderer/core/xml/xml_serializer.cc

#ifndef MITTO_DOM_XML_SERIALIZER_H_
#define MITTO_DOM_XML_SERIALIZER_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class Node;

class XMLSerializer final : public bindings::ScriptWrappable {
 public:
  static XMLSerializer* Create();

  XMLSerializer();
  ~XMLSerializer() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string SerializeToString(Node* root) const;
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_SERIALIZER_H_
