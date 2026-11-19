#include "mitto/dom/dom_parser.h"

#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_dom_parser.h"
#include "mitto/dom/document.h"
#include "mitto/dom/html_document.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
DOMParser* DOMParser::Create() {
  return bindings::MakeGarbageCollected<DOMParser>(Window::Current());
}

DOMParser::DOMParser(Window& window) : window_(&window) {}

DOMParser::~DOMParser() = default;

const bindings::WrapperTypeInfo* DOMParser::GetWrapperTypeInfo() const {
  return &bindings::V8DOMParser::kWrapperTypeInfo;
}

void DOMParser::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(window_);
  bindings::ScriptWrappable::Trace(visitor);
}

Document* DOMParser::ParseFromString(
    const std::u16string& string,
    const std::u16string& type,
    bindings::ExceptionState& exception_state) {
  if (type != u"text/html") {
    exception_state.ThrowTypeError(
        bindings::ExceptionMessages::NotImplemented());
    return nullptr;
  }
  // DocumentInit with the window's URL and the window as its execution
  // context, which gives the window's origin, and no frame.
  const Document& creator = window_->GetDocument();
  HTMLDocument* const document = HTMLDocument::Create();
  document->SetUrl(creator.UrlForBinding());
  document->SetOrigin(creator.GetOrigin());
  document->SetAllowDeclarativeShadowRoots(false);
  document->SetContent(string);
  return document;
}

}  // namespace mitto
