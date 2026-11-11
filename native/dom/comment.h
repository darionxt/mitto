// The DOM's Comment node.
//
// Upstream: third_party/blink/renderer/core/dom/comment.h
// Upstream: third_party/blink/renderer/core/dom/comment.cc

#ifndef MITTO_DOM_COMMENT_H_
#define MITTO_DOM_COMMENT_H_

#include <string>

#include "mitto/dom/character_data.h"

namespace mitto {

class Comment final : public CharacterData {
 public:
  // new Comment(data), in the current Window's document.
  static Comment* Create(const std::u16string& data);
  static Comment* Create(Document& document, const std::u16string& data);

  Comment(Document& document, const std::u16string& data);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

 protected:
  Node* CloneWithoutChildren(Document& document) const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_COMMENT_H_
