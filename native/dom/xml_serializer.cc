#include "mitto/dom/xml_serializer.h"

#include "base/check.h"
#include "mitto/bindings/v8_xml_serializer.h"
#include "mitto/dom/markup_serializer.h"
#include "mitto/dom/node.h"

namespace mitto {

// static
XMLSerializer* XMLSerializer::Create() {
  return bindings::MakeGarbageCollected<XMLSerializer>();
}

XMLSerializer::XMLSerializer() = default;

XMLSerializer::~XMLSerializer() = default;

const bindings::WrapperTypeInfo* XMLSerializer::GetWrapperTypeInfo() const {
  return &bindings::V8XMLSerializer::kWrapperTypeInfo;
}

std::u16string XMLSerializer::SerializeToString(Node* root) const {
  DCHECK(root);
  return SerializeNodeAsXml(*root);
}

}  // namespace mitto
