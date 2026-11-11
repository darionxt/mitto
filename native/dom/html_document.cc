#include "mitto/dom/html_document.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_document.h"

namespace mitto {

// static
HTMLDocument* HTMLDocument::Create() {
  return bindings::MakeGarbageCollected<HTMLDocument>();
}

HTMLDocument::HTMLDocument() : Document(/*is_html=*/true) {}

HTMLDocument::~HTMLDocument() = default;

const bindings::WrapperTypeInfo* HTMLDocument::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLDocument::kWrapperTypeInfo;
}

}  // namespace mitto
