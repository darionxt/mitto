// The DOM's CDATASection node: a Text node of a document that is not HTML,
// which createCDATASection() makes.
//
// Upstream: third_party/blink/renderer/core/dom/cdata_section.h
// Upstream: third_party/blink/renderer/core/dom/cdata_section.cc

#ifndef MITTO_DOM_CDATA_SECTION_H_
#define MITTO_DOM_CDATA_SECTION_H_

#include <string>

#include "mitto/dom/text.h"

namespace mitto {

class CDATASection final : public Text {
 public:
  static CDATASection* Create(Document& document, const std::u16string& data);

  CDATASection(Document& document, const std::u16string& data);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

 protected:
  Text* CloneWithData(Document& document,
                      const std::u16string& data) const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_CDATA_SECTION_H_
