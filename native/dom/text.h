// The DOM's Text node.
//
// Upstream: third_party/blink/renderer/core/dom/text.h
// Upstream: third_party/blink/renderer/core/dom/text.cc

#ifndef MITTO_DOM_TEXT_H_
#define MITTO_DOM_TEXT_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/character_data.h"

namespace mitto {

// Not final: CDATASection is a Text, as in Blink.
class Text : public CharacterData {
 public:
  // new Text(data), in the current Window's document.
  static Text* Create(const std::u16string& data);
  static Text* Create(Document& document, const std::u16string& data);

  Text(Document& document, const std::u16string& data);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  Text* SplitText(uint32_t offset, bindings::ExceptionState& exception_state);
  std::u16string WholeText() const;

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

 protected:
  Node* CloneWithoutChildren(Document& document) const override;
  // Blink's CloneWithData(): a node of this one's kind with `data`, which
  // splitText() and cloning make, so that a CDATA section's are CDATA
  // sections.
  virtual Text* CloneWithData(Document& document,
                              const std::u16string& data) const;
};

}  // namespace mitto

#endif  // MITTO_DOM_TEXT_H_
