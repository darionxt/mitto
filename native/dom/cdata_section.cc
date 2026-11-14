#include "mitto/dom/cdata_section.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_cdata_section.h"

namespace mitto {

// static
CDATASection* CDATASection::Create(Document& document,
                                   const std::u16string& data) {
  return bindings::MakeGarbageCollected<CDATASection>(document, data);
}

CDATASection::CDATASection(Document& document, const std::u16string& data)
    : Text(document, data) {}

const bindings::WrapperTypeInfo* CDATASection::GetWrapperTypeInfo() const {
  return &bindings::V8CDATASection::kWrapperTypeInfo;
}

uint16_t CDATASection::GetNodeType() const {
  return kCdataSectionNode;
}

std::u16string CDATASection::NodeName() const {
  return u"#cdata-section";
}

Node* CDATASection::CloneWithoutChildren(Document& document) const {
  return Create(document, Data());
}

}  // namespace mitto
