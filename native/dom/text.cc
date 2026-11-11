#include "mitto/dom/text.h"

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_text.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
Text* Text::Create(const std::u16string& data) {
  return Create(Window::Current().GetDocument(), data);
}

// static
Text* Text::Create(Document& document, const std::u16string& data) {
  return bindings::MakeGarbageCollected<Text>(document, data);
}

Text::Text(Document& document, const std::u16string& data)
    : CharacterData(document, data) {}

const bindings::WrapperTypeInfo* Text::GetWrapperTypeInfo() const {
  return &bindings::V8Text::kWrapperTypeInfo;
}

Text* Text::SplitText(uint32_t offset,
                      bindings::ExceptionState& exception_state) {
  if (offset > Length()) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The offset ", base::NumberToString(offset),
                      " is larger than the Text node's length."}));
    return nullptr;
  }
  Text* const next = Create(GetDocument(), Data().substr(offset));
  SetData(Data().substr(0, offset));
  if (Node* const parent = ParentNode()) {
    if (Node* const sibling = NextSibling()) {
      parent->ParserInsertBefore(next, *sibling);
    } else {
      parent->ParserAppendChild(next);
    }
  }
  return next;
}

std::u16string Text::WholeText() const {
  // The data of the Text nodes next to this one with no other node between.
  const Node* start = this;
  while (start->PreviousSibling() && start->PreviousSibling()->IsTextNode()) {
    start = start->PreviousSibling();
  }
  std::u16string text;
  for (const Node* node = start; node && node->IsTextNode();
       node = node->NextSibling()) {
    text += static_cast<const Text*>(node)->Data();
  }
  return text;
}

Node* Text::CloneWithoutChildren(Document& document) const {
  return Create(document, Data());
}

uint16_t Text::GetNodeType() const {
  return kTextNode;
}

std::u16string Text::NodeName() const {
  return u"#text";
}

}  // namespace mitto
