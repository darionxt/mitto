#include "mitto/dom/comment.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_comment.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
Comment* Comment::Create(const std::u16string& data) {
  return Create(Window::Current().GetDocument(), data);
}

// static
Comment* Comment::Create(Document& document, const std::u16string& data) {
  return bindings::MakeGarbageCollected<Comment>(document, data);
}

Comment::Comment(Document& document, const std::u16string& data)
    : CharacterData(document, data) {}

const bindings::WrapperTypeInfo* Comment::GetWrapperTypeInfo() const {
  return &bindings::V8Comment::kWrapperTypeInfo;
}

Node* Comment::CloneWithoutChildren(Document& document) const {
  return Create(document, Data());
}

uint16_t Comment::GetNodeType() const {
  return kCommentNode;
}

std::u16string Comment::NodeName() const {
  return u"#comment";
}

}  // namespace mitto
